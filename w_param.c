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


/// Внутреннее хранение таблицы параметров
static const w_param_descriptor_t *g_param_table = NULL;
static size_t                      g_param_count = 0;
static bool                        g_initialized = false;

/**
 * @brief Callback для приёма данных на канале W_CHAN_PARAMS
 * @details
 *   Вызывается при новом событии (новые данные в канале). Внутри коллбека
 *   необходимо получить блок через Rdt_ReceiveBlock().
 */
static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data);

/**
 * @brief Обработка одного пакета (вырезано из callback’а в отдельную функцию для удобства)
 */
static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size);

/**
 * @brief Поиск описателя параметра по message_type
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

/************************************************************************
 *             Реализация внешних функций
 ************************************************************************/

void w_param_init(const w_param_descriptor_t *table, size_t table_count)
{
    g_param_table = table;
    g_param_count = table_count;
    g_initialized = true;
}

void w_param_deinit(void)
{
    g_param_table = NULL;
    g_param_count = 0;
    g_initialized = false;
}

void w_param_start(void)
{
    if (!g_initialized)
    {
        // Защита от ситуации, когда не вызвали w_param_init
        // Либо здесь можно за логировать ошибку
        return;
    }
    // Регистрируем коллбек на канал, предназначенный для параметров
    Wireless_Channel_Receive_Callback_Register(w_param_receive_cb, W_CHAN_PARAMS);
}

int w_param_send_request(uint8_t message_type,
                         uint8_t set_or_get,
                         const uint8_t *value,
                         size_t value_len)
{
    // Формируем пакет с заголовком
    size_t full_size = sizeof(w_header_param_t) + value_len;
    w_header_param_t *hdr = (w_header_param_t *)malloc(full_size);
    if (!hdr)
    {
        // Ошибка выделения памяти
        return 1;
    }

    hdr->message_type = message_type;
    hdr->set_or_get   = set_or_get;
    hdr->return_code  = 0;  // При запросе ставим 0; при ответе будет меняться

    if (value && value_len > 0)
    {
        memcpy(hdr->data, value, value_len);
    }

    // Отправляем блок по каналу
    int ret = Rdt_SendBlock(W_CHAN_PARAMS,
                            (const uint8_t *)hdr,
                            full_size,
                            NULL);
    free(hdr);

    return ret;
}

/************************************************************************
 *             Локальные (static) функции
 ************************************************************************/

static void w_param_receive_cb(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data)
{
    // По условию: при событии нужно извлечь блок из очереди
    rdt_block_item_t block_item;
    if (!Rdt_ReceiveBlock(W_CHAN_PARAMS, &block_item, 0))
    {
        // Нет данных - возможно, ложный вызов
        // или ошибка приёма
        logE("w_param_receive_cb: no block received");
        return;
    }

    if (block_item.data_ptr && block_item.data_size > 0)
    {
        w_param_process_packet(block_item.data_ptr, block_item.data_size);
    }
    else
    {
        logE("w_param_receive_cb: empty block");
    }

    // Освобождаем блок
    Rdt_FreeReceivedBlock(&block_item);
}

static void w_param_process_packet(const uint8_t *packet_data, size_t packet_size)
{
    if (packet_size < sizeof(w_header_param_t))
    {
        logE("w_param_process_packet: invalid packet size");
        return;
    }

    // Парсим заголовок
    const w_header_param_t *hdr_in = (const w_header_param_t *)packet_data;
    uint8_t message_type = hdr_in->message_type;
    uint8_t set_or_get   = hdr_in->set_or_get;

    // Ищем в реестре
    const w_param_descriptor_t *desc = find_param_descriptor(message_type);
    if (!desc)
    {
        // Параметр не найден
        // Отправим ответ с return_code != 0 (например, 1)
        w_header_param_t hdr_out;
        hdr_out.message_type = message_type;
        hdr_out.set_or_get   = set_or_get;
        hdr_out.return_code  = 1; // код ошибки "нет такого параметра"

        // Ответ без данных
        Rdt_SendBlock(W_CHAN_PARAMS, (const uint8_t *)&hdr_out, sizeof(hdr_out), NULL);
        return;
    }

    // Если это запрос GET/SET (или ответ), нам нужно сформировать ответ.
    // По условию задачи, предполагается двусторонний обмен: запрос -> ответ,
    // где тот же message_type используется для ответа.
    uint8_t return_code = 0;

    // Память для ответа. Сформируем максимально возможный ответ,
    // вдруг при чтении вернётся сколько-то байт.
    // Поскольку неизвестно, какой максимум – условно возьмём 256 как некий предел
    // или см. специфику параметра. Для примера – 256.
    // В реальном коде можно использовать динамику.
    size_t max_response_size = MAX_PARAM_LENGHT;
    uint8_t *response_buf = (uint8_t *)malloc(sizeof(w_header_param_t) + max_response_size);
    if (!response_buf)
    {
        logE("w_param_process_packet: no mem for response");
        return;
    }

    // Заполняем заголовок для ответа
    w_header_param_t *hdr_out = (w_header_param_t *)response_buf;
    hdr_out->message_type = message_type;
    hdr_out->set_or_get   = set_or_get;

    // При SET-запросе входные данные начинаются после заголовка:
    const uint8_t *payload_in  = hdr_in->data;
    size_t payload_in_size     = packet_size - sizeof(w_header_param_t);

    // Подготовка для GET-ответа
    uint8_t *payload_out       = hdr_out->data;
    size_t   payload_out_size  = max_response_size;
    memset(payload_out, 0, payload_out_size);

    // Определяем логику
    if (set_or_get == W_PARAM_GET)
    {
        // Проверяем, поддерживается ли чтение
        if (desc->read_fn)
        {
            return_code = desc->read_fn(payload_out, &payload_out_size);
        }
        else
        {
            return_code = 2; // ошибка, нет функции чтения
            payload_out_size = 0;
        }
    }
    else if (set_or_get == W_PARAM_SET)
    {
        // Проверяем, поддерживается ли запись
        if (desc->write_fn)
        {
            return_code = desc->write_fn(payload_in, payload_in_size);
            // При успешной записи бывает нужно вернуть текущее значение
            // или нулевые данные - зависит от протокола. Допустим, возвращаем ничего.
            payload_out_size = 0;
        }
        else
        {
            return_code = 3; // ошибка, нет функции записи
            payload_out_size = 0;
        }
    }
    else
    {
        // неизвестный тип запроса
        return_code = 4; 
        payload_out_size = 0;
    }

    hdr_out->return_code = return_code;

    // Итоговый размер ответа
    size_t total_response_size = sizeof(w_header_param_t) + payload_out_size;

    // Отправляем ответ
    Rdt_SendBlock(W_CHAN_PARAMS, response_buf, total_response_size, NULL);

    free(response_buf);
}
