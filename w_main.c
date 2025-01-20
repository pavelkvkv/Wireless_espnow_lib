/**
 * @file w_main.c
 * @brief Основной файл библиотеки wireless_lib для ESP32.
 *
 * Обеспечивает соединение двух ESP32 по WiFi. В зависимости от роли
 * устройство может быть либо точкой доступа, либо клиентом.
 * Оба режима организуют сокетное соединение и обмениваются данными.
 * @author pavel
 * @date 2025-01-18
 */

#include "w_main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_system.h"

// ========================= Константы и настройки ==========================

/**
 * @brief Максимальное количество логических каналов
 */
#define RDT_MAX_CHANNELS        4

/**
 * @brief Размер полезной нагрузки каждого пакета, байт
 */
#define RDT_PACKET_PAYLOAD_LEN  192

/**
 * @brief Полный размер пакета (байт) в плане структуры
 *        (Заголовок + payload + CRC).
 *        При необходимости можно оставить запас, чтобы не превышать 250 байт.
 */
#define RDT_PACKET_TOTAL_SIZE   250

/**
 * @brief Таймаут ожидания ACK (ASK) перед повторной пересылкой всего блока, мс
 */
#define RDT_ACK_TIMEOUT_MS      100

/**
 * @brief Максимальное количество повторных отправок целого блока
 */
#define RDT_MAX_RETRY_COUNT     5

/**
 * @brief Коды служебных сообщений
 */
typedef enum
{
    RDT_MSG_BEGIN = 1,  // Начало передачи
    RDT_MSG_DATA,       // Обычный пакет данных
    RDT_MSG_END,        // Конец передачи
    RDT_MSG_ASK,        // Все пакеты получены
    RDT_MSG_NACK        // Запрос на переотправку
} rdt_service_code_t;

// ========================= Структуры данных ==========================

/**
 * @brief Структура пакета, передаваемого по ESP-NOW
 */
typedef struct
{
    uint8_t  channel;                         ///< Номер логического канала
    uint16_t seq_num;                         ///< Порядковый номер пакета
    uint8_t  service_code;                    ///< Служебный код
    uint8_t  payload[RDT_PACKET_PAYLOAD_LEN]; ///< Полезная нагрузка
    uint32_t crc;                             ///< CRC (не входит в расчёт самого CRC)
} __attribute__((packed)) rdt_packet_t;



/**
 * @brief Внутреннее состояние канала для приёма
 */
typedef struct
{
    bool     receiving;           ///< Флаг активного приёма
    size_t   total_size;          ///< Ожидаемый размер всего блока в байтах
    uint16_t total_packets;       ///< Кол-во пакетов в блоке
    uint16_t packets_received;    ///< Сколько пакетов уже принято
    uint8_t *rx_buffer;           ///< Указатель на буфер для сборки всего блока
    bool    *packet_received_map; ///< Флаги приёма отдельных пакетов
    int64_t  last_packet_time;    ///< Метка времени последнего принятого пакета
} rdt_channel_rx_t;

/**
 * @brief Внутреннее состояние канала для передачи
 */
typedef struct
{
    bool     sending;             ///< Флаг активной отправки
    size_t   current_size;        ///< Текущий размер блока
    uint16_t total_packets;       ///< Общее кол-во пакетов для блока
    uint8_t *tx_buffer;           ///< Исходный блок данных
    uint8_t  retry_count;         ///< Счётчик повторных отправок всего блока
    uint16_t next_seq_to_send;    ///< Какой seq отправлять следующим (в общей последовательности)
    bool    *packet_sent_map;     ///< Флаги того, какие пакеты уже отправлялись
    int64_t  last_send_time;      ///< Метка времени последнего события отправки
} rdt_channel_tx_t;

/**
 * @brief Описание одного логического канала (TX и RX части + очереди)
 */
typedef struct
{
    // Очередь приёма блоков (уже собранных)
    QueueHandle_t rx_queue;
    // Очередь отправки блоков (сырьё, которое нужно передать)
    QueueHandle_t tx_queue;
    // Управление приёмом
    rdt_channel_rx_t rx_ctrl;
    // Управление передачей
    rdt_channel_tx_t tx_ctrl;
    // Размер очередей
    uint8_t rx_queue_length;
    uint8_t tx_queue_length;
    // Максимальный размер одного блока данных в байтах (должен делиться на 192, если хочется ровно)
    size_t  max_block_size;
} rdt_channel_t;

// ========================= Глобальные/статические переменные ==========================

static const char *TAG = "rdt";

/**
 * @brief Массив логических каналов
 */
static rdt_channel_t s_channels[RDT_MAX_CHANNELS] = {0};

/**
 * @brief Мьютекс для защиты общих структур
 */
static SemaphoreHandle_t s_rdt_mutex = NULL;

/**
 * @brief Широковещательный MAC
 * (внешний код может установить нужный адрес или использовать для всех)
 */
static uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/**
 * @brief Внутренняя очередь для событий приёма/отправки, которые обрабатываются в задаче
 */
typedef enum
{
    RDT_EVENT_SEND_OK,
    RDT_EVENT_SEND_FAIL, // Для ESP-NOW при неуспехе (но в LR может не отрабатывать)
    RDT_EVENT_RECV_PKT
} rdt_internal_event_type_t;

typedef struct
{
    rdt_internal_event_type_t event_type; ///< Тип события
    rdt_packet_t packet;                  ///< Копия принятого пакета (для RDT_EVENT_RECV_PKT)
    uint8_t src_mac[6];                  ///< MAC отправителя
} rdt_event_msg_t;

static QueueHandle_t s_rdt_event_queue = NULL;
static TaskHandle_t  s_rdt_task_handle = NULL;
static u8            s_peer_macaddr[6] = {0};

// ========================= Прототипы статических функций ==========================

/** @brief Коллбек ESP-NOW для отправки */
static void rdt_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status);

/** @brief Коллбек ESP-NOW для приёма */
static void rdt_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);

/** @brief Основная задача RDT для обработки событий */
static void rdt_task(void *arg);

/** @brief Подготовка и отправка одного пакета */
static esp_err_t rdt_send_one_packet(uint8_t channel_idx, uint16_t seq, rdt_service_code_t code, 
                                     const uint8_t *payload, size_t payload_len);

/** @brief Формирование CRC пакета */
static uint32_t rdt_calc_crc(const rdt_packet_t *pkt);

/** @brief Обработчик принятого пакета */
static void rdt_process_received_packet(uint8_t channel_idx, const rdt_packet_t *pkt, const uint8_t *src_mac);

/** @brief Обработка логики передачи (текущего блока) */
static void rdt_process_tx_channel(uint8_t channel_idx);

/** @brief Перезапуск (повторная отправка) всего блока с начала */
static void rdt_restart_tx_block(uint8_t channel_idx);

/** @brief Поиск пропущенных пакетов и формирование nack */
static void rdt_send_nack_for_missing(uint8_t channel_idx, const uint8_t *dst_mac);

// ========================= Определения статических функций ==========================

static void rdt_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (!mac_addr) return;
    rdt_event_msg_t msg = {0};
    msg.event_type = (status == ESP_NOW_SEND_SUCCESS) ? RDT_EVENT_SEND_OK : RDT_EVENT_SEND_FAIL;
    xQueueSendFromISR(s_rdt_event_queue, &msg, NULL);
}

/**
 * @brief Коллбек приема данных по ESP-NOW
 * @param[in] recv_info Информация о принимающем устройстве (MAC-адрес, RSSI и т.д.)
 * @param[in] data Указатель на принятые данные
 * @param[in] len Длина принятых данных
 */
static void rdt_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!recv_info || !data || len < (int)sizeof(rdt_packet_t))
    {
        // Если данные некорректны, выходим
        return;
    }

    // Считаем, что данные не превышают 250 байт, а структура rdt_packet_t соответствует этим данным.
    // Создаём сообщение для обработки в основной задаче.
    rdt_event_msg_t msg = {0};
    msg.event_type = RDT_EVENT_RECV_PKT;

    // Копируем MAC-адрес отправителя
    memcpy(msg.src_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    // Копируем данные пакета (если длина подходит для структуры)
    if (len >= (int)sizeof(rdt_packet_t))
    {
        memcpy(&msg.packet, data, sizeof(rdt_packet_t));
    }

    // Отправляем сообщение в очередь событий, чтобы обработка происходила в задаче rdt_task
    if (xQueueSendFromISR(s_rdt_event_queue, &msg, NULL) != pdTRUE)
    {
        // Если очередь переполнена, обработка будет пропущена
        ESP_LOGW(TAG, "Event queue full, packet dropped");
    }
}

static void rdt_task(void *arg)
{
    (void)arg;
    rdt_event_msg_t event = {0};

    while (true)
    {
        if (xQueueReceive(s_rdt_event_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            xSemaphoreTake(s_rdt_mutex, portMAX_DELAY);

            switch (event.event_type)
            {
            case RDT_EVENT_SEND_OK:
            case RDT_EVENT_SEND_FAIL:
                // В LR-режиме это может не давать реальной информации, но код оставим.
                // При необходимости можно использовать для статистики.
                break;

            case RDT_EVENT_RECV_PKT:
                // Обработка принятого пакета
                if (event.packet.channel < RDT_MAX_CHANNELS)
                {
                    rdt_process_received_packet(event.packet.channel, &event.packet, event.src_mac);
                }
                break;
            default:
                break;
            }

            // Периодический проход по каналам для логики передачи
            for (uint8_t i = 0; i < RDT_MAX_CHANNELS; i++)
            {
                rdt_process_tx_channel(i);
            }

            xSemaphoreGive(s_rdt_mutex);
        }
        else
        {
            // Таймаут очереди: всё равно периодически обходим каналы
            xSemaphoreTake(s_rdt_mutex, portMAX_DELAY);
            for (uint8_t i = 0; i < RDT_MAX_CHANNELS; i++)
            {
                rdt_process_tx_channel(i);
            }
            xSemaphoreGive(s_rdt_mutex);
        }
    }
}

static esp_err_t rdt_send_one_packet(uint8_t channel_idx, uint16_t seq, rdt_service_code_t code, 
                                     const uint8_t *payload, size_t payload_len)
{
    if (channel_idx >= RDT_MAX_CHANNELS) return ESP_ERR_INVALID_ARG;
    if (payload_len > RDT_PACKET_PAYLOAD_LEN) return ESP_ERR_INVALID_SIZE;

    rdt_packet_t pkt = {0};
    pkt.channel      = channel_idx;
    pkt.seq_num      = seq;
    pkt.service_code = (uint8_t)code;

    if (payload && payload_len > 0)
    {
        memcpy(pkt.payload, payload, payload_len);
    }

    pkt.crc = rdt_calc_crc(&pkt);

    // Отправка по ESP-NOW
    return esp_now_send(s_peer_macaddr, (uint8_t*)&pkt, sizeof(rdt_packet_t));
}

static uint32_t rdt_calc_crc(const rdt_packet_t *pkt)
{
    // Поле CRC не должно входить в расчёт, поэтому используем размер без последнего поля
    // Считаем: sizeof(rdt_packet_t) - sizeof(pkt->crc) = ...
    uint32_t crc_len = sizeof(rdt_packet_t) - sizeof(pkt->crc);
    return esp_crc32_le(UINT32_MAX, (const uint8_t*)pkt, crc_len);
}

static void rdt_process_received_packet(uint8_t channel_idx, const rdt_packet_t *pkt, const uint8_t *src_mac)
{
    // Проверяем CRC
    uint32_t calc_crc = rdt_calc_crc(pkt);
    if (calc_crc != pkt->crc)
    {
        // CRC не совпал — игнорируем
        return;
    }

    rdt_channel_t    *ch  = &s_channels[channel_idx];
    rdt_channel_rx_t *rx  = &ch->rx_ctrl;
    rdt_channel_tx_t *tx  = &ch->tx_ctrl; // Для некоторых типов пакетов (nack, ask) нужна передача
    
    switch (pkt->service_code)
    {
    case RDT_MSG_BEGIN:
    {
        // Начало приёма на этом канале
        rx->receiving       = true;
        rx->packets_received = 0;
        // Предположим, что размер блока передаётся в первых байтах payload (если нужно),
        // либо в любом другом формате. Для примера — пусть там лежит 4 байта размера.
        if (pkt->payload[0] || pkt->payload[1] || pkt->payload[2] || pkt->payload[3])
        {
            rx->total_size = 
                ((size_t)pkt->payload[0])       |
                ((size_t)pkt->payload[1] << 8)  |
                ((size_t)pkt->payload[2] << 16) |
                ((size_t)pkt->payload[3] << 24);
        }
        else
        {
            // Если нет ясен размер, выставим максимум
            rx->total_size = ch->max_block_size;
        }
        rx->total_packets    = (rx->total_size + RDT_PACKET_PAYLOAD_LEN - 1) / RDT_PACKET_PAYLOAD_LEN + 2; // +2 c учётом begin/end
        // Освобождаем старые буферы, если что
        if (rx->rx_buffer)
        {
            free(rx->rx_buffer);
            rx->rx_buffer = NULL;
        }
        if (rx->packet_received_map)
        {
            free(rx->packet_received_map);
            rx->packet_received_map = NULL;
        }
        // Выделяем новые буферы
        rx->rx_buffer          = (uint8_t*)calloc(1, rx->total_size);
        rx->packet_received_map = (bool*)calloc(rx->total_packets, sizeof(bool));
        // Сразу фиксируем приём пакета BEGIN
        rx->packet_received_map[pkt->seq_num] = true;
        rx->packets_received                  = 1;
        rx->last_packet_time                  = esp_timer_get_time(); // microseconds
        break;
    }

    case RDT_MSG_DATA:
    {
        if (!rx->receiving) return; // Не в режиме приёма
        if (pkt->seq_num >= rx->total_packets) 
        {
            // seq_num выходит за рамки
            return;
        }
        if (!rx->packet_received_map[pkt->seq_num])
        {
            rx->packet_received_map[pkt->seq_num] = true;
            rx->packets_received++;
            // Копируем payload
            size_t offset = (pkt->seq_num - 1) * RDT_PACKET_PAYLOAD_LEN; 
            // seq_num - 1, т.к. 0 — это BEGIN, а начиная с 1 идут data
            size_t copy_len = RDT_PACKET_PAYLOAD_LEN;
            if (offset + copy_len > rx->total_size)
            {
                copy_len = rx->total_size - offset;
            }
            if (offset < rx->total_size)
            {
                memcpy(rx->rx_buffer + offset, pkt->payload, copy_len);
            }
        }
        rx->last_packet_time = esp_timer_get_time();
        // Проверим, не надо ли отправить NACK. Если не пришли какие-то пакеты до seq_num?
        // Для упрощения отправим NACK только по таймеру или при получении "end".
        break;
    }

    case RDT_MSG_END:
    {
        if (!rx->receiving) return;
        if (pkt->seq_num != rx->total_packets - 1)
        {
            // seq не совпадает с последним
            return;
        }
        // Помечаем, что получили последний
        if (!rx->packet_received_map[pkt->seq_num])
        {
            rx->packet_received_map[pkt->seq_num] = true;
            rx->packets_received++;
        }
        // Проверяем, все ли пакеты
        bool all_ok = (rx->packets_received == rx->total_packets);
        if (!all_ok)
        {
            // Отправляем NACK
            rdt_send_nack_for_missing(channel_idx, src_mac);
        }
        else
        {
            // Всё собрано, отправляем ASK
            rdt_send_one_packet(channel_idx, 0, RDT_MSG_ASK, NULL, 0);
            // Складываем блок в rx-очередь
            rdt_block_item_t completed_block;
            memset(&completed_block, 0, sizeof(completed_block));
            completed_block.data_ptr  = rx->rx_buffer;
            completed_block.data_size = rx->total_size;
            xQueueSend(ch->rx_queue, &completed_block, 0);
            // Обнуляем
            rx->rx_buffer          = NULL;
            rx->packet_received_map = NULL;
            rx->receiving          = false;
        }
        rx->last_packet_time = esp_timer_get_time();
        break;
    }

    case RDT_MSG_ASK:
    {
        // Приёмник подтверждает, что все пакеты получены
        if (tx->sending)
        {
            // Завершаем передачу блока, освобождаем буферы
            free(tx->packet_sent_map);
            tx->packet_sent_map = NULL;
            free(tx->tx_buffer);
            tx->tx_buffer       = NULL;
            tx->sending         = false;
            ESP_LOGI(TAG, "Channel %d: block transmitted successfully", channel_idx);
        }
        break;
    }

    case RDT_MSG_NACK:
    {
        // В payload могут быть номера seq для повторной отправки
        if (tx->sending)
        {
            // Пробежимся по списку seq
            // Для простоты считаем, что первые N байт payload — это список seq (по 2 байта).
            int count = (RDT_PACKET_PAYLOAD_LEN / 2);
            for (int i = 0; i < count; i++)
            {
                uint16_t missing_seq = ((uint16_t)pkt->payload[2*i]) | ((uint16_t)pkt->payload[2*i+1] << 8);
                if (missing_seq == 0xFFFF) // Можем условиться о конце списка
                {
                    break;
                }
                // Переотправляем
                if (missing_seq < tx->total_packets)
                {
                    if (missing_seq == 0)
                    {
                        // begin
                        uint8_t size_arr[4];
                        size_arr[0] = (uint8_t)((tx->current_size >> 0) & 0xFF);
                        size_arr[1] = (uint8_t)((tx->current_size >> 8) & 0xFF);
                        size_arr[2] = (uint8_t)((tx->current_size >> 16) & 0xFF);
                        size_arr[3] = (uint8_t)((tx->current_size >> 24) & 0xFF);
                        rdt_send_one_packet(channel_idx, 0, RDT_MSG_BEGIN, size_arr, 4);
                    }
                    else if (missing_seq == (tx->total_packets - 1))
                    {
                        // end
                        rdt_send_one_packet(channel_idx, missing_seq, RDT_MSG_END, NULL, 0);
                    }
                    else
                    {
                        // data
                        size_t offset = (missing_seq - 1) * RDT_PACKET_PAYLOAD_LEN;
                        size_t chunk_len = RDT_PACKET_PAYLOAD_LEN;
                        if (offset + chunk_len > tx->current_size)
                        {
                            chunk_len = tx->current_size - offset;
                        }
                        rdt_send_one_packet(channel_idx, missing_seq, RDT_MSG_DATA, tx->tx_buffer + offset, chunk_len);
                    }
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

static void rdt_process_tx_channel(uint8_t channel_idx)
{
    rdt_channel_t    *ch = &s_channels[channel_idx];
    rdt_channel_tx_t *tx = &ch->tx_ctrl;

    if (!tx->sending)
    {
        // Проверяем, есть ли задача передать блок
        if (uxQueueMessagesWaiting(ch->tx_queue) > 0)
        {
            rdt_block_item_t block_item;
            if (xQueueReceive(ch->tx_queue, &block_item, 0) == pdTRUE)
            {
                // Инициализируем передачу
                tx->sending      = true;
                tx->retry_count  = 0;
                tx->current_size = block_item.data_size;
                tx->tx_buffer    = block_item.data_ptr; // Передаём владение
                tx->total_packets = (tx->current_size + RDT_PACKET_PAYLOAD_LEN - 1) / RDT_PACKET_PAYLOAD_LEN + 2; // +2: BEGIN, END
                tx->packet_sent_map = (bool*)calloc(tx->total_packets, sizeof(bool));
                tx->next_seq_to_send = 0;
                tx->last_send_time   = esp_timer_get_time();

                // Отправим begin
                uint8_t size_arr[4];
                size_arr[0] = (uint8_t)((tx->current_size >> 0) & 0xFF);
                size_arr[1] = (uint8_t)((tx->current_size >> 8) & 0xFF);
                size_arr[2] = (uint8_t)((tx->current_size >> 16) & 0xFF);
                size_arr[3] = (uint8_t)((tx->current_size >> 24) & 0xFF);

                rdt_send_one_packet(channel_idx, 0, RDT_MSG_BEGIN, size_arr, 4);
                tx->packet_sent_map[0] = true;
                tx->next_seq_to_send   = 1;
            }
        }
    }
    else
    {
        // Проверяем таймаут на получение ASK
        int64_t now = esp_timer_get_time();
        if ((now - tx->last_send_time) > (RDT_ACK_TIMEOUT_MS * 1000))
        {
            // Не получили ASK: переотправляем весь блок
            tx->retry_count++;
            if (tx->retry_count >= RDT_MAX_RETRY_COUNT)
            {
                // Сдаёмся — сбрасываем передачу
                ESP_LOGW(TAG, "Channel %d: block send failed after max retries", channel_idx);
                free(tx->packet_sent_map);
                tx->packet_sent_map = NULL;
                free(tx->tx_buffer);
                tx->tx_buffer = NULL;
                tx->sending   = false;
            }
            else
            {
                rdt_restart_tx_block(channel_idx);
            }
            return;
        }

        // Если ещё остались неотправленные пакеты, отправляем
        while (tx->next_seq_to_send < tx->total_packets)
        {
            // Отправим следующий пакет, если он ещё не отправлялся
            if (!tx->packet_sent_map[tx->next_seq_to_send])
            {
                if (tx->next_seq_to_send == (tx->total_packets - 1))
                {
                    // END
                    rdt_send_one_packet(channel_idx, tx->next_seq_to_send, RDT_MSG_END, NULL, 0);
                }
                else
                {
                    // DATA
                    size_t offset = (tx->next_seq_to_send - 1) * RDT_PACKET_PAYLOAD_LEN;
                    size_t chunk_len = RDT_PACKET_PAYLOAD_LEN;
                    if (offset + chunk_len > tx->current_size)
                    {
                        chunk_len = tx->current_size - offset;
                    }
                    rdt_send_one_packet(channel_idx, tx->next_seq_to_send, RDT_MSG_DATA, tx->tx_buffer + offset, chunk_len);
                }
                tx->packet_sent_map[tx->next_seq_to_send] = true;
                tx->last_send_time = esp_timer_get_time();
            }
            tx->next_seq_to_send++;
        }
    }
}

static void rdt_restart_tx_block(uint8_t channel_idx)
{
    rdt_channel_tx_t *tx = &s_channels[channel_idx].tx_ctrl;
    ESP_LOGW(TAG, "Channel %d: re-send entire block", channel_idx);
    memset(tx->packet_sent_map, 0, tx->total_packets * sizeof(bool));
    tx->next_seq_to_send = 0;
    // Отправим begin
    uint8_t size_arr[4];
    size_arr[0] = (uint8_t)((tx->current_size >> 0) & 0xFF);
    size_arr[1] = (uint8_t)((tx->current_size >> 8) & 0xFF);
    size_arr[2] = (uint8_t)((tx->current_size >> 16) & 0xFF);
    size_arr[3] = (uint8_t)((tx->current_size >> 24) & 0xFF);

    rdt_send_one_packet(channel_idx, 0, RDT_MSG_BEGIN, size_arr, 4);
    tx->packet_sent_map[0] = true;
    tx->next_seq_to_send   = 1;
    tx->last_send_time     = esp_timer_get_time();
}

static void rdt_send_nack_for_missing(uint8_t channel_idx, const uint8_t *dst_mac)
{
    rdt_channel_t    *ch = &s_channels[channel_idx];
    rdt_channel_rx_t *rx = &ch->rx_ctrl;

    // Собираем список пропущенных seq
    // Для простоты пусть в payloadе идут seq (2 байта на seq), а 0xFFFF — конец.
    uint8_t buffer[RDT_PACKET_PAYLOAD_LEN] = {0};
    int     idx = 0;
    for (uint16_t i = 0; i < rx->total_packets; i++)
    {
        if (!rx->packet_received_map[i])
        {
            if (idx + 2 <= RDT_PACKET_PAYLOAD_LEN)
            {
                buffer[idx]   = (uint8_t)(i & 0xFF);
                buffer[idx+1] = (uint8_t)((i >> 8) & 0xFF);
                idx += 2;
            }
            else
            {
                break;
            }
        }
    }
    // Добавим 0xFFFF в конец
    if (idx + 2 <= RDT_PACKET_PAYLOAD_LEN)
    {
        buffer[idx]   = 0xFF;
        buffer[idx+1] = 0xFF;
    }
    rdt_send_one_packet(channel_idx, 0, RDT_MSG_NACK, buffer, RDT_PACKET_PAYLOAD_LEN);
}

// ========================= Глобальные функции ==========================

/**
 * @brief Инициализация ESP-NOW и RDT
 * @return ESP_OK или ESP_FAIL
 */
int Wireless_Init(void)
{
    // Здесь оставляем ту логику, что была, или меняем под себя

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));

    // Инициализация очередей и мьютекса для RDT
    if (!s_rdt_mutex)
    {
        s_rdt_mutex = xSemaphoreCreateMutex();
    }
    if (!s_rdt_event_queue)
    {
        s_rdt_event_queue = xQueueCreate(10, sizeof(rdt_event_msg_t));
    }
    // Запуск задачи RDT
    if (!s_rdt_task_handle)
    {
        xTaskCreate(rdt_task, "rdt_task", 4096, NULL, 5, &s_rdt_task_handle);
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(rdt_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(rdt_recv_cb));
    // Установим PMK (пароль для шифрования, при необходимости)
    uint8_t pmk[16] = {0}; // или реальный ключ
    ESP_ERROR_CHECK(esp_now_set_pmk(pmk));

    // Добавление широковещательного пира
    Rdt_AddPeer(s_broadcast_mac);

    // Настройка сохранённого пира
    S_MC_Get_Paired_Display_id(s_peer_macaddr);
    if(s_peer_macaddr[0] == 0)
    {
        logW("No paired display");
        memcpy(s_peer_macaddr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    }
    else
    {
        Rdt_AddPeer(s_peer_macaddr);
    }

    ESP_LOGI(TAG, "ESP-NOW и RDT инициализированы");
    return ESP_OK;
}

/**
 * @brief Регистрация/создание очередей для одного логического канала
 * @param[in] channel Номер канала (0..RDT_MAX_CHANNELS-1)
 * @param[in] rx_queue_len Длина очереди приёма в элементах
 * @param[in] tx_queue_len Длина очереди передачи в элементах
 * @param[in] max_block_size Максимальный размер блока данных (в байтах)
 * @return 0 - OK, 1 - ошибка
 */
int Rdt_ChannelInit(uint8_t channel, uint8_t rx_queue_len, uint8_t tx_queue_len, size_t max_block_size)
{
    if (channel >= RDT_MAX_CHANNELS) return 1;
    rdt_channel_t *ch = &s_channels[channel];

    if (!ch->rx_queue)
    {
        ch->rx_queue = xQueueCreate(rx_queue_len, sizeof(rdt_block_item_t));
        ch->rx_queue_length = rx_queue_len;
    }
    if (!ch->tx_queue)
    {
        ch->tx_queue = xQueueCreate(tx_queue_len, sizeof(rdt_block_item_t));
        ch->tx_queue_length = tx_queue_len;
    }
    ch->max_block_size = max_block_size;
    return 0;
}

/**
 * @brief Добавить блок (указатель на данные) на отправку
 * @param[in] channel Номер канала
 * @param[in] data_ptr Указатель на блок данных
 * @param[in] size Размер блока данных
 * @param[in] user_ctx Пользовательский контекст (необязательно)
 * @return 0 - OK, 1 - ошибка
 */
int Rdt_SendBlock(uint8_t channel, const uint8_t *data_ptr, size_t size, void *user_ctx)
{
    if (channel >= RDT_MAX_CHANNELS) return 1;
    if (!data_ptr || size == 0) return 1;

    rdt_channel_t *ch = &s_channels[channel];

    rdt_block_item_t item;
    item.data_ptr  = (uint8_t*)data_ptr; // ВНИМАНИЕ: передаём владение!
    item.data_size = size;
    item.user_ctx  = user_ctx;

    if (xQueueSend(ch->tx_queue, &item, 0) != pdTRUE)
    {
        // Очередь заполнена
        return 1;
    }
    return 0;
}

/**
 * @brief Получить готовый принятый блок из rx-очереди (если есть)
 * @param[in]  channel Номер канала
 * @param[out] block_item Указатель на структуру для приёма результата
 * @param[in]  wait_ticks Время ожидания (в тиках)
 * @return true, если блок получен, false - если таймаут
 */
bool Rdt_ReceiveBlock(uint8_t channel, rdt_block_item_t *block_item, TickType_t wait_ticks)
{
    if (channel >= RDT_MAX_CHANNELS) return false;
    if (!block_item) return false;

    rdt_channel_t *ch = &s_channels[channel];
    if (xQueueReceive(ch->rx_queue, block_item, wait_ticks) == pdTRUE)
    {
        return true;
    }
    return false;
}

/**
 * @brief Освободить принятый блок, если библиотека выделяла память
 * @param[in] block_item Блок, полученный через Rdt_ReceiveBlock
 */
void Rdt_FreeReceivedBlock(rdt_block_item_t *block_item)
{
    if (!block_item) return;
    if (block_item->data_ptr)
    {
        free(block_item->data_ptr);
    }
    block_item->data_ptr  = NULL;
    block_item->data_size = 0;
    block_item->user_ctx  = NULL;
}

/**
 * @brief Зарегистрировать peer по MAC (если не использовать широковещание)
 * @param[in] peer_mac Указатель на MAC (6 байт)
 */
void Rdt_AddPeer(const uint8_t *peer_mac)
{
    if (!peer_mac) return;
    esp_now_peer_info_t peer = {0};
    peer.channel = 1; // или другой канал
    peer.ifidx   = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer);
    memcpy(s_peer_macaddr, peer_mac, ESP_NOW_ETH_ALEN);
}

