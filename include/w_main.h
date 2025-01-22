#ifndef W_MAIN_H
#define W_MAIN_H

#include "../../main/include/my_types.h"
#include "freertos/FreeRTOS.h"
#include "esp_event.h"

extern esp_event_base_t const WIRELESS_EVENT_BASE;
extern esp_event_loop_handle_t W_event_loop;

// ========================= Константы и настройки ==========================

/**
 * @brief Максимальное количество логических каналов
 */
#define RDT_MAX_CHANNELS        4

// ========================= Структуры данных ==========================

/**
 * @brief Элемент очереди для отправки/приёма целого блока
 */
typedef struct
{
    uint8_t *data_ptr;            ///< Указатель на массив данных (блок)
    size_t   data_size;           ///< Размер всего блока данных
    void    *user_ctx;            ///< Пользовательский контекст (необязательное поле)
} rdt_block_item_t;

// ========================= Публичные функции ==========================

/**
 * @brief Инициализация ESP-NOW и RDT
 * @return ESP_OK или ESP_FAIL
 */
int Wireless_Init(void);

/**
 * @brief Регистрация/создание очередей для одного логического канала
 * @param[in] channel Номер канала (0..RDT_MAX_CHANNELS-1)
 * @param[in] rx_queue_len Длина очереди приёма в элементах
 * @param[in] tx_queue_len Длина очереди передачи в элементах
 * @param[in] max_block_size Максимальный размер блока данных (в байтах)
 * @return 0 - OK, 1 - ошибка
 */
int Rdt_ChannelInit(uint8_t channel, uint8_t rx_queue_len, uint8_t tx_queue_len, size_t max_block_size);

/**
 * @brief Добавить блок (указатель на данные) на отправку
 * @param[in] channel Номер канала
 * @param[in] data_ptr Указатель на блок данных (будет автомпатически освобождаться)
 * @param[in] size Размер блока данных
 * @param[in] user_ctx Пользовательский контекст (необязательно)
 * @return 0 - OK, 1 - ошибка
 */
int Rdt_SendBlock(uint8_t channel, const uint8_t *data_ptr, size_t size, void *user_ctx);

/**
 * @brief Получить готовый принятый блок из rx-очереди (если есть)
 * @param[in]  channel Номер канала
 * @param[out] block_item Указатель на структуру для приёма результата
 * @param[in]  wait_ticks Время ожидания (в тиках)
 * @return true, если блок получен, false - если таймаут
 */
bool Rdt_ReceiveBlock(uint8_t channel, rdt_block_item_t *block_item, TickType_t wait_ticks);

/**
 * @brief Освободить принятый блок, если библиотека выделяла память
 * @param[in] block_item Блок, полученный через Rdt_ReceiveBlock
 */
void Rdt_FreeReceivedBlock(rdt_block_item_t *block_item);

/**
 * @brief Зарегистрировать peer по MAC (если не использовать широковещание)
 * @param[in] peer_mac Указатель на MAC (6 байт)
 */
void Rdt_AddPeer(const uint8_t *peer_mac);

void Wireless_Channel_Clear_Queue(int channel);

/**
 * @brief Get the current RSSI value
 * @return RSSI value or 0 if not available
 */
int Wireless_Rssi_Get(void);

enum 
{
    CON_NOT_PAIRED = 0,
    CON_PAIRED,
    CON_PAIRING_ACTIVE,
    CON_CONNECTED,
    CON_DISCONNECTED
};

typedef enum {
    EVENT_TYPE_1,
    EVENT_TYPE_2,
    EVENT_TYPE_3,
    EVENT_TYPE_4
} my_event_t;



#endif // W_MAIN_H  