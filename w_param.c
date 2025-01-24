/**
 *  @file w_param.c
 *  @brief Реализация библиотеки для обмена параметрами (например, через ESP-NOW).
 *
 *  @details
 *  Этот файл содержит реализацию функций для отправки/получения параметров.
 *  Он предполагает использование внешнего драйвера (rdt_xxx) для низкоуровневой отправки/приёма данных.
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


/* ----------------------------------------------------------------
 * Глобальные/статические переменные
 * ---------------------------------------------------------------- */

// Таблица параметров
static const w_param_descriptor_t *g_param_table = NULL;
static size_t                      g_param_count = 0;
static bool                        g_initialized = false;

// Для блокирующего запроса/ответа:
static SemaphoreHandle_t g_response_sem = NULL; ///< Семафор для ожидания ответа
static bool              g_request_in_progress = false; ///< Флаг активного запроса

static SemaphoreHandle_t g_request_mutex = NULL;

/**
 * Поле message_type текущего запроса (чтобы знать, какой ответ ожидаем)
 */
static uint8_t g_req_msg_type  = 0;

/**
 * Буфер для данных ответа и указатель на поле с размером
 */
static uint8_t  *g_resp_user_buf  = NULL;
static size_t   *g_resp_user_size = NULL;

/**
 * Код возврата из ответа
 */
static uint8_t   g_resp_return_code = 255;

/**
 * Фактическая длина данных в ответе
 */
static size_t    g_resp_data_len = 0;

/* ----------------------------------------------------------------
 * Предварительные объявления
 * ---------------------------------------------------------------- */

static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size);
static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data);

static const w_param_descriptor_t* find_param_descriptor(uint8_t message_type)
{
    for (size_t i = 0; i < g_param_count; i++)
    {
        if (g_param_table[i].message_type == message_type)
            return &g_param_table[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Реализация публичных функций
 * ---------------------------------------------------------------- */

void w_param_init(const w_param_descriptor_t *table, size_t table_count)
{
    g_param_table = table;
    g_param_count = table_count;
    g_initialized = true;

    // Создание семафора для ожидания ответа
    if (!g_response_sem)
    {
        g_response_sem = xSemaphoreCreateBinary();
    }
    g_request_mutex = xSemaphoreCreateMutex();
}

void w_param_deinit(void)
{
    g_param_table = NULL;
    g_param_count = 0;
    g_initialized = false;
    // Опционально: удаление семафора
    // vSemaphoreDelete(g_response_sem);
    // g_response_sem = NULL;
}

void w_param_start(void)
{
    if (!g_initialized)
    {
        logE("w_param_start: модуль не инициализирован!");
        return;
    }
    // Регистрация callback для приёма пакетов
    Wireless_Channel_Receive_Callback_Register(w_param_receive_cb, W_CHAN_PARAMS);
}

int w_param_send_request_async(uint8_t message_type,
                               uint8_t set_or_get,
                               const uint8_t *value,
                               size_t value_len)
{
    // Выделяем память для исходящего пакета
    size_t full_size = sizeof(w_header_param_t) + value_len;
    w_header_param_t *hdr = (w_header_param_t *)malloc(full_size);
    if (!hdr)
    {
        return 1; // Ошибка выделения памяти
    }

    hdr->message_type = message_type;
    hdr->set_or_get   = set_or_get; // W_PARAM_GET или W_PARAM_SET
    hdr->return_code  = 0;

    if (value && value_len > 0)
    {
        memcpy(hdr->data, value, value_len);
    }

    // Отправляем пакет
    int ret = Rdt_SendBlock(W_CHAN_PARAMS, (const uint8_t *)hdr, full_size, NULL);
    if (ret == 1)
    {
        // Если ошибка отправки (1), освобождаем память
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
    logI("Param request msg_type=%d, %s, value_len=%d", message_type, set_or_get?"SET":"GET", value_len);
    if (!g_initialized)
    {
        if (return_code) { *return_code = 0xFF; }
        logE("модуль не инициализирован!");
        return -1;
    }

    if(xSemaphoreTake(g_request_mutex, W_PARAM_DEFAULT_TIMEOUT ) != pdTRUE)
    {
        if (return_code) { *return_code = 0xFE; }
        logE("mutex take failed!");
        return -2;
    }

    if (g_request_in_progress)
    {
        if (return_code) { *return_code = 0xFE; }
        logE("уже выполняется другой запрос!");
        xSemaphoreGive(g_request_mutex);
        return -2;
    }

    // Подготовка к выполнению нового запроса
    g_request_in_progress = true;
    g_req_msg_type        = message_type;
    g_resp_return_code    = 0xFF;
    g_resp_data_len       = 0;
    g_resp_user_buf       = resp_data;
    g_resp_user_size      = resp_size;

    // Очистка семафора
    xSemaphoreTake(g_response_sem, 0);

    // Отправка запроса
    int ret_send = w_param_send_request_async(message_type, set_or_get, value, value_len);
    if (ret_send != 0)
    {
        // Ошибка отправки
        g_request_in_progress = false;
        if (return_code) { *return_code = 0xFD; }
        logW("ошибка при отправке запроса");
        xSemaphoreGive(g_request_mutex);
        return ret_send; 
    }

    // Ожидание ответа или таймаута
    if (xSemaphoreTake(g_response_sem, wait_ticks) == pdTRUE)
    {
        // Ответ получен
        if (return_code)
            *return_code = g_resp_return_code;

        g_request_in_progress = false;
        logI("Done param request msg_type=%d, %s, value_len=%d", message_type, set_or_get?"SET":"GET", value_len);
        xSemaphoreGive(g_request_mutex);
        return 0;
    }
    else
    {
        // Таймаут ожидания ответа
        g_request_in_progress = false;
        if (return_code) { *return_code = 0xFC; }
        logW("превышено время ожидания ответа");
        xSemaphoreGive(g_request_mutex);
        return -3;
    }
}

int w_param_get(uint8_t message_type,
                uint8_t *resp_data,
                size_t *resp_size,
                uint8_t *return_code)
{
    return w_param_request_blocking(message_type,
                                    W_PARAM_GET,
                                    NULL,
                                    0,
                                    resp_data,
                                    resp_size,
                                    W_PARAM_DEFAULT_TIMEOUT,
                                    return_code);
}

int w_param_set(uint8_t message_type,
                const uint8_t *value,
                size_t value_len,
                uint8_t *return_code)
{
    return w_param_request_blocking(message_type,
                                    W_PARAM_SET,
                                    value,
                                    value_len,
                                    NULL,
                                    NULL,
                                    W_PARAM_DEFAULT_TIMEOUT,
                                    return_code);
}

/* ----------------------------------------------------------------
 * Реализация локальных функций
 * ---------------------------------------------------------------- */

/**
 * @brief Callback-функция, вызываемая при получении блока данных в канале W_CHAN_PARAMS
 */
static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data)
{
    (void)handler_arg;
    (void)base;
    (void)id;
    (void)event_data;

    rdt_block_item_t block_item;
    if (!Rdt_ReceiveBlock(W_CHAN_PARAMS, &block_item, 0))
    {
        logE("блок данных отсутствует");
        return;
    }

    if (block_item.data_ptr && block_item.data_size >= sizeof(w_header_param_t))
    {
        w_param_process_packet(block_item.data_ptr, block_item.data_size);
    }
    else
    {
        logE("некорректный блок данных");
    }

    Rdt_FreeReceivedBlock(&block_item);
}

/**
 * @brief Обработка входящего пакета (запроса или ответа)
 */
static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size)
{
    const w_header_param_t *hdr_in = (const w_header_param_t *)packet_data;
    if (packet_size < sizeof(w_header_param_t))
    {
        logE("слишком короткий пакет");
        return;
    }

    uint8_t message_type = hdr_in->message_type;
    uint8_t set_or_get   = hdr_in->set_or_get;
    uint8_t return_code  = 0; 

    // Выделяем полезную нагрузку входящего пакета
    const uint8_t *payload_in = hdr_in->data;
    size_t payload_in_size = packet_size - sizeof(w_header_param_t);

    /* -----------------------------------------------------
       Если это запрос (W_PARAM_GET или W_PARAM_SET),
       обрабатываем его и отправляем ответ W_PARAM_RESP.
       ----------------------------------------------------- */
    if (set_or_get == W_PARAM_GET || set_or_get == W_PARAM_SET)
    {
        // Ищем дескриптор параметра
        const w_param_descriptor_t *desc = find_param_descriptor(message_type);
        if (!desc)
        {
            // Параметр не найден
            return_code = 1; // код ошибки: "параметр не найден"
            // Формируем ответ с кодом ошибки
            size_t resp_len = sizeof(w_header_param_t);
            w_header_param_t *resp_pkt = (w_header_param_t *)malloc(resp_len);
            if (!resp_pkt) return;

            resp_pkt->message_type = message_type;
            resp_pkt->set_or_get   = W_PARAM_RESP; // это ответ
            resp_pkt->return_code  = return_code;
            
            int ret = Rdt_SendBlock(W_CHAN_PARAMS, (uint8_t*)resp_pkt, resp_len, NULL);
            if (ret == 1) free(resp_pkt);
            return;
        }

        // Готовим буфер для ответа (например, до 256 байт)
        size_t max_resp_data = MAX_PARAM_LENGTH;
        uint8_t *resp_buf = (uint8_t *)malloc(sizeof(w_header_param_t) + max_resp_data);
        if (!resp_buf)
        {
            logE("недостаточно памяти для ответа");
            return;
        }

        w_header_param_t *hdr_out = (w_header_param_t *)resp_buf;
        hdr_out->message_type = message_type;
        hdr_out->set_or_get   = W_PARAM_RESP; // отмечаем как ответ

        uint8_t *payload_out = hdr_out->data;
        size_t   payload_out_size = max_resp_data;
        memset(payload_out, 0, payload_out_size);

        // Выполняем чтение или запись
        if (set_or_get == W_PARAM_GET)
        {
            if (desc->read_fn)
            {
                return_code = desc->read_fn(payload_out, &payload_out_size);
            }
            else
            {
                return_code = 2; // чтение не поддерживается
                payload_out_size = 0;
            }
        }
        else // W_PARAM_SET
        {
            if (desc->write_fn)
            {
                return_code = desc->write_fn(payload_in, payload_in_size);
                // Если требуется, можно вернуть актуальное значение, но здесь для простоты этого нет
                payload_out_size = 0;
            }
            else
            {
                return_code = 3; // запись не поддерживается
                payload_out_size = 0;
            }
        }

        hdr_out->return_code = return_code;

        // Отправляем ответ
        size_t total_out_size = sizeof(w_header_param_t) + payload_out_size;
        int ret = Rdt_SendBlock(W_CHAN_PARAMS, resp_buf, total_out_size, NULL);
        if (ret == 1)
        {
            free(resp_buf);
        }
    }
    /* -----------------------------------------------------
       Если это W_PARAM_RESP, то это ответ на наш запрос.
       Проверяем, был ли активный запрос с таким message_type.
       ----------------------------------------------------- */
    else if (set_or_get == W_PARAM_RESP)
    {
        if (g_request_in_progress && (message_type == g_req_msg_type))
        {
            g_resp_return_code = hdr_in->return_code;

            // Копируем данные ответа в пользовательский буфер, если задан
            if (g_resp_user_buf && g_resp_user_size)
            {
                size_t to_copy = (payload_in_size <= *g_resp_user_size)
                               ? payload_in_size
                               : *g_resp_user_size;
                memcpy(g_resp_user_buf, payload_in, to_copy);
                *g_resp_user_size = to_copy;
                g_resp_data_len   = to_copy;
            }
            else if (g_resp_user_size)
            {
                *g_resp_user_size = payload_in_size;
                g_resp_data_len   = payload_in_size;
            }

            // Освобождаем ожидающий поток
            xSemaphoreGive(g_response_sem);
        }
        else
        {
            // logW("Неожиданный или ненужный ответ type=%d", (int)message_type);
            // if(g_request_in_progress) logW("Ожидается ответ на запрос type=%d", (int)g_req_msg_type);
            // else logW("Нет активного запроса");
        }
    }
    else
    {
        logW("неизвестный тип пакета set_or_get=%d", (int)set_or_get);
    }
}
