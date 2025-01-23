/**
 *  @file w_param.c
 *  @brief Реализация библиотеки для обмена параметрами по ESP-NOW
 *
 *  @details
 *  Данный файл содержит реализацию библиотеки для обмена параметрами по ESP-NOW.
 *  Он включает в себя функции для отправки и приёма параметров.
 *  Общий файл библиотеки (на все устройства)
 * 
 *  @author Pavel
 *  @date 2025-01-22
 */

#include "w_param.h"
#include "wireless_port.h"   
#include "w_main.h"
#include "w_user.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "w_param"
#include "../../main/include/log.h"


/* ------------------------------------------
 * Глобальные/статические переменные
 * ------------------------------------------ */

// Таблица параметров, заданная извне
static const w_param_descriptor_t *g_param_table = NULL;
static size_t                      g_param_count = 0;
static bool                        g_initialized = false;

/*
 * Для реализации блокирующего запроса «запрос-ответ», нужно хранить:
 *  1) Флаг, что сейчас есть «висящий» запрос.
 *  2) Какой message_type и set_or_get мы ждём в ответе.
 *  3) Буфер и код возврата, куда поместим ответ, когда он придёт.
 *  4) Семафор (или другое средство синхронизации), чтобы «разбудить» вызывающий поток.
 *
 * В данном примере всё хранится в статических переменных (1 запрос за раз).
 * Если захотите поддерживать параллельные запросы - потребуется усложнить логику (например, используя очередь).
 */

static SemaphoreHandle_t g_response_sem = NULL;  ///< Семафор для синхронного ожидания
static bool              g_request_in_progress = false;
static uint8_t           g_req_msg_type = 0;
static uint8_t           g_req_set_or_get = 0;
static uint8_t          *g_resp_user_buf = NULL; ///< Указатель на буфер, куда нужно сложить ответ
static size_t           *g_resp_user_size = NULL;///< Указатель на размер буфера (in/out)
static uint8_t           g_resp_return_code = 255; ///< Временное хранилище кода возврата
static size_t            g_resp_data_len = 0;   ///< Сколько байт реального ответа пришло

/**
 * @brief Поиск дескриптора параметра в таблице по message_type
 */
static const w_param_descriptor_t * find_param_descriptor(uint8_t message_type)
{
    for (size_t i = 0; i < g_param_count; i++)
    {
        if (g_param_table[i].message_type == message_type)
        {
            return &g_param_table[i];
        }
    }
    return NULL; // не найден
}

/**
 * @brief Обработка входящего блока с параметрами
 * @param[in] packet_data   Указатель на буфер данных
 * @param[in] packet_size   Размер буфера
 */
static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size);

/**
 * @brief Коллбек, вызываемый при появлении данных в канале W_CHAN_PARAMS
 */
static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data);


/* ------------------------------------------
 * Реализация внешних функций
 * ------------------------------------------ */

void w_param_init(const w_param_descriptor_t *table, size_t table_count)
{
    g_param_table = table;
    g_param_count = table_count;
    g_initialized = true;

    // Создадим двоичный семафор для ожидания ответа
    if (!g_response_sem)
    {
        g_response_sem = xSemaphoreCreateBinary();
    }
}

void w_param_deinit(void)
{
    g_param_table = NULL;
    g_param_count = 0;
    g_initialized = false;
    // Семафор по желанию можно удалить
    // vSemaphoreDelete(g_response_sem); 
    // g_response_sem = NULL;
}

void w_param_start(void)
{
    if (!g_initialized)
    {
        logE("w_param_start: not initialized!");
        return;
    }
    // Регистрируем коллбек на канал
    Wireless_Channel_Receive_Callback_Register(w_param_receive_cb, W_CHAN_PARAMS);
}

int w_param_send_request_async(uint8_t message_type,
                               uint8_t set_or_get,
                               const uint8_t *value,
                               size_t value_len)
{
    // Асинхронная отправка без ожидания
    size_t full_size = sizeof(w_header_param_t) + value_len;
    w_header_param_t *hdr = (w_header_param_t *)malloc(full_size);
    if (!hdr)
    {
        return 1; // ошибка выделения памяти
    }

    hdr->message_type = message_type;
    hdr->set_or_get   = set_or_get;
    hdr->return_code  = 0;

    if (value && value_len > 0)
    {
        memcpy(hdr->data, value, value_len);
    }

logI("1");
    int ret = Rdt_SendBlock(W_CHAN_PARAMS, (const uint8_t *)hdr, full_size, NULL);
    if (ret == 1)
    {
        free(hdr);
    }
    return ret;
}

int w_param_request_blocking(uint8_t message_type,
                             uint8_t set_or_get,
                             const uint8_t *value,
                             size_t value_len,
                             uint8_t *resp_data,
                             size_t *resp_size,
                             TickType_t wait_ticks,
                             uint8_t *return_code)
{
    logI("Param Request: message_type=%02X, %s, len=%d", message_type, set_or_get?"SET":"GET", (int)value_len);
    if (!g_initialized)
    {
        logE("w_param_request_blocking: not initialized!");
        if (return_code) { *return_code = 0xFF; }
        return -1;
    }

    // Защита от параллельных запросов
    if (g_request_in_progress)
    {
        logE("w_param_request_blocking: another request is in progress!");
        if (return_code) { *return_code = 0xFE; }
        return -2;
    }

    // Сброс/подготовка внутренних переменных
    g_request_in_progress = true;
    g_req_msg_type        = message_type;
    g_req_set_or_get      = set_or_get;
    g_resp_return_code    = 0xFF;
    g_resp_data_len       = 0;
    g_resp_user_buf       = resp_data;
    g_resp_user_size      = resp_size;

    // Перед отправкой обнуляем семафор (если был «зависший»)
    xSemaphoreTake(g_response_sem, 0);

    // Отправим пакет запроса (используем внутреннюю функцию)
    int ret_send = w_param_send_request_async(message_type, set_or_get, value, value_len);
    if (ret_send != 0)
    {
        // Ошибка при отправке
        g_request_in_progress = false;
        if (return_code) { *return_code = 0xFD; }
        logW("error sending request");
        return ret_send; 
    }

    // Ожидаем освобождения семафора (т.е. прихода ответа) или таймаута
    if (xSemaphoreTake(g_response_sem, wait_ticks) == pdTRUE)
    {
        // Ответ пришёл
        if (return_code)
        {
            *return_code = g_resp_return_code;
        }
        // g_resp_data_len уже скопирован в пользовательский буфер внутри коллбэка
        // Здесь можно дополнительно вернуть фактический размер:
        if (resp_size) 
        {
            // В resp_size уже записан фактический размер в коллбэке
        }

        g_request_in_progress = false;
        logI("Param got OK");
        return 0; // всё OK
    }
    else
    {
        // Таймаут
        g_request_in_progress = false;
        if (return_code) { *return_code = 0xFC; }
        logW("Param timeout");
        return -3; // код ошибки по таймауту
    }
}

/**
 * @brief Обёртка для выполнения GET-запроса с использованием стандартного таймаута.
 * @param[in]  message_type  Тип параметра (W_MSG_TYPE_PARAM_XXX)
 * @param[out] resp_data     Буфер для полученных данных
 * @param[in,out] resp_size  Размер буфера на входе, фактический размер на выходе
 * @param[out] return_code   Код возврата из ответа (0 - успех, != 0 - ошибка)
 *
 * @return 0 - успех (ответ получен), != 0 - ошибка (например, таймаут или внутренняя ошибка)
 */
int w_param_get(uint8_t message_type,
               uint8_t *resp_data,
               size_t *resp_size,
               uint8_t *return_code)
{
    return w_param_request_blocking(
        message_type,       // message_type
        W_PARAM_GET,        // set_or_get
        NULL,               // value (NULL for GET)
        0,                  // value_len
        resp_data,          // resp_data
        resp_size,          // resp_size
        W_PARAM_DEFAULT_TIMEOUT, // wait_ticks
        return_code         // return_code
    );
}

/**
 * @brief Обёртка для выполнения SET-запроса с использованием стандартного таймаута.
 * @param[in]  message_type  Тип параметра (W_MSG_TYPE_PARAM_XXX)
 * @param[in]  value         Данные для установки
 * @param[in]  value_len     Размер данных для установки
 * @param[out] return_code   Код возврата из ответа (0 - успех, != 0 - ошибка)
 *
 * @return 0 - успех (ответ получен), != 0 - ошибка (например, таймаут или внутренняя ошибка)
 */
int w_param_set(uint8_t message_type,
               const uint8_t *value,
               size_t value_len,
               uint8_t *return_code)
{
    return w_param_request_blocking(
        message_type,       // message_type
        W_PARAM_SET,        // set_or_get
        value,              // value (data for SET)
        value_len,          // value_len
        NULL,               // resp_data (optional, can be NULL)
        NULL,               // resp_size (optional, can be NULL)
        W_PARAM_DEFAULT_TIMEOUT, // wait_ticks
        return_code         // return_code
    );
}


/************************************************************************
 *             Локальные (static) функции
 ************************************************************************/

static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)id;
    (void)event_data;

    // Получаем блок
    rdt_block_item_t block_item;
    if (!Rdt_ReceiveBlock(W_CHAN_PARAMS, &block_item, 0))
    {
        logE("w_param_receive_cb: no block to receive");
        return;
    }

    if (block_item.data_ptr && block_item.data_size >= sizeof(w_header_param_t))
    {
        w_param_process_packet(block_item.data_ptr, block_item.data_size);
    }
    else
    {
        logE("w_param_receive_cb: invalid block");
    }

    // Освобождаем
    Rdt_FreeReceivedBlock(&block_item);
}

static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size)
{
    const w_header_param_t *hdr_in = (const w_header_param_t *)packet_data;
    if (packet_size < sizeof(w_header_param_t))
    {
        logE("w_param_process_packet: packet too short");
        return;
    }

    uint8_t message_type = hdr_in->message_type;
    uint8_t set_or_get   = hdr_in->set_or_get;
    uint8_t return_code  = 0; // будет установлен по результату функции чтения/записи

    // Вычисляем «полезную» часть входного пакета
    const uint8_t *payload_in = hdr_in->data;
    size_t         payload_in_size = packet_size - sizeof(w_header_param_t);

    // Ищем дескриптор
    const w_param_descriptor_t *desc = find_param_descriptor(message_type);

    // Если не нашли, формируем ответ (если это запрос).
    // Если это ответ, то, возможно, кто-то ждал его синхронно. Но по протоколу
    // «ответ» и «запрос» выглядят одинаково, только тот, кто формировал запрос,
    // понимает, что это – ответ. В нашем случае распознаём так:
    //  - Если в g_request_in_progress == true, и (message_type, set_or_get) совпадают,
    //    значит это может быть ответ. И наоборот.
    // Для простоты определим: если параметр не найден и это запрос (GET/SET) – отсылаем ошибку.
    // Если это «ответ», кто-то ждал – сообщим туда «нет такого параметра».
    if (!desc)
    {
        return_code = 1; // код ошибки: «нет такого параметра»
        // Отправим ответ, только если пришёл именно запрос GET или SET
        // (иначе можно считать пакет «мусором»)
        bool is_request = ((set_or_get == W_PARAM_GET) || (set_or_get == W_PARAM_SET));
        if (is_request)
        {
            // Формируем и шлём ответ
            w_header_param_t *hdr_out = (w_header_param_t *)malloc(sizeof(w_header_param_t));
            hdr_out->message_type = message_type;
            hdr_out->set_or_get   = set_or_get;
            hdr_out->return_code  = return_code;
            logI("2");
            if (1 == Rdt_SendBlock(W_CHAN_PARAMS, (const uint8_t *)hdr_out, sizeof(hdr_out), NULL))
            {
                free(hdr_out);
            }
        }

        // Также, если кто-то ждал ответ (с такими же поля ми?), нужно «разбудить»
        // Но без дескриптора – всё равно ошибка. Проверим совпадение:
        if (g_request_in_progress &&
            g_req_msg_type == message_type &&
            g_req_set_or_get == set_or_get)
        {
            // Будем считать, что это «ответ» с ошибкой
            g_resp_return_code = return_code;
            // 0 байт данных
            if (g_resp_user_size) 
            {
                *g_resp_user_size = 0;
            }
            // Сигналим
            xSemaphoreGive(g_response_sem);
        }
        return;
    }

    // Если это GET или SET (запрос от удалённого устройства), обрабатываем и шлём ответ.
    if (set_or_get == W_PARAM_GET || set_or_get == W_PARAM_SET)
    {
        // Готовим буфер для ответа
        // (допустим, макс. 256, либо динамический подход)
        size_t max_resp_data = 256;
        uint8_t *resp_buf = (uint8_t *)malloc(sizeof(w_header_param_t) + max_resp_data);
        if (!resp_buf)
        {
            logE("w_param_process_packet: no mem for response");
            return;
        }

        // Заполним заголовок
        w_header_param_t *hdr_out = (w_header_param_t *)resp_buf;
        hdr_out->message_type = message_type;
        hdr_out->set_or_get   = set_or_get;

        // Указатель на тело ответа:
        uint8_t *payload_out = hdr_out->data;
        size_t   payload_out_size = max_resp_data;
        memset(payload_out, 0, payload_out_size);

        // Выполним чтение/запись
        if (set_or_get == W_PARAM_GET)
        {
            if (desc->read_fn)
            {
                // read_fn заполнит payload_out, укажет фактический размер
                return_code = desc->read_fn(payload_out, &payload_out_size);
            }
            else
            {
                return_code = 2; // нет функции чтения
                payload_out_size = 0;
            }
        }
        else // W_PARAM_SET
        {
            if (desc->write_fn)
            {
                return_code = desc->write_fn(payload_in, payload_in_size);
                // Если нужно вернуть актуальное значение — можно вызвать read_fn 
                // или просто вернуть пустые данные. Для примера — вернём ничего.
                payload_out_size = 0;
            }
            else
            {
                return_code = 3; // нет функции записи
                payload_out_size = 0;
            }
        }

        hdr_out->return_code = return_code;

        // Отправляем ответ
        size_t total_out_size = sizeof(w_header_param_t) + payload_out_size;
        logI("3");
        if (1 == Rdt_SendBlock(W_CHAN_PARAMS, resp_buf, total_out_size, NULL))
        {
            free(resp_buf);
        }
    }
    else
    {
        // Иначе, считаем, что это «ответ» (или неведомый тип).
        // Для простоты предполагаем, что если set_or_get не 0 и не 1, это может быть «ответ»,
        // но в реальности тот же заголовок используется и для ответа. 
        // Пришедший пакет *может* быть ответом на наш запрос — проверим:
        if (g_request_in_progress &&
            g_req_msg_type == message_type &&
            g_req_set_or_get == set_or_get)
        {
            // Совпадение. Копируем данные (если пользователь попросил).
            return_code = hdr_in->return_code;
            g_resp_return_code = return_code;

            if (g_resp_user_buf && g_resp_user_size)
            {
                // Посчитаем, сколько есть «полезных данных»:
                size_t rcv_data_len = payload_in_size; 
                // Не превышаем размер пользовательского буфера
                if (rcv_data_len > *g_resp_user_size)
                {
                    rcv_data_len = *g_resp_user_size;
                }
                memcpy(g_resp_user_buf, payload_in, rcv_data_len);
                *g_resp_user_size = rcv_data_len; // сообщаем, сколько реально записали
                g_resp_data_len   = rcv_data_len;
            }
            else if (g_resp_user_size)
            {
                // Пользовательский буфер не задан, но resp_size задан — 
                // значит скажем размер, но не копируем
                *g_resp_user_size = payload_in_size;
                g_resp_data_len   = payload_in_size;
            }
            else
            {
                // Пользователю не нужны данные
            }

            // Разбуживаем заблокировавшуюся задачу
            xSemaphoreGive(g_response_sem);
        }
        else
        {
            // Либо это ответ не на наш запрос, либо вообще мусор.
            // Можно залогировать.
            // logI("w_param_process_packet: unhandled or unsolicited packet");
        }
    }
}