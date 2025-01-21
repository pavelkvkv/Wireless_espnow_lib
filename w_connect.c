/**
 * @file w_connect.c
 * @author Pavel
 * @brief Полная реализация привязки двух устройств с помощью espnow, используя структуру для сообщений
 * @date 2025-01-20
 *
 * Данный модуль реализует процедуру привязки (pairing) двух устройств, обменивающихся
 * MAC-адресами и подтверждающих факт сохранения адреса друг друга. Для передачи используется 
 * канал W_CHAN_SYSTEM. Алгоритм обеспечивает защиту от «частичной» привязки, когда 
 * только одно устройство сохранило MAC-адрес, а второе — нет.
 */

#include "w_main.h"
#include "w_user.h"
#include "wireless_port.h"
#include "esp_event.h"
#include "esp_mac.h"
#include <string.h>
#include <stdlib.h>

#define TAG "w_connect"
#include "../../main/include/log.h"

/*------------------------------------------------------------------------------
 * Внешние объявления или заглушки, если нужно
 *----------------------------------------------------------------------------*/

// Предполагаем, что эти функции/переменные определены где-то в проекте:
// extern void S_MC_Set_Paired_Display_id(const uint8_t *mac);
// extern void S_MC_Get_Paired_Display_id(uint8_t *mac_out);
// extern void S_Commit_All(void);

// extern bool Rdt_SendBlock(uint8_t channel, void *data_ptr, size_t size, void *user_arg);
// extern bool Rdt_ReceiveBlock(uint8_t channel, rdt_block_item_t *out_block, uint32_t timeout_ms);
// extern void Rdt_FreeReceivedBlock(rdt_block_item_t *block);
// extern void Rdt_AddPeer(const uint8_t *mac);




/*------------------------------------------------------------------------------
 * Локальные переменные
 *----------------------------------------------------------------------------*/

/** Флаг активности процесса привязки */
static bool pairing_active = false;

/** Временное хранение MAC-адреса потенциального пира */
static uint8_t temp_peer_mac[6] = {0};
static bool    have_temp_peer   = false;

/** Флаг «получили подтверждение, что нас тоже сохранили» */
static bool got_done_from_peer  = false;

/*------------------------------------------------------------------------------
 * Прототипы локальных функций
 *----------------------------------------------------------------------------*/
static void   wireless_pairing_receive_cb(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data);
static void   wireless_pairing_task(void *arg);
static void   finalize_pairing(void);
static void   revert_pairing(void);

/*------------------------------------------------------------------------------
 * Функции, доступные извне
 *----------------------------------------------------------------------------*/

/**
 * @brief Получить статус привязки
 * @return CON_NOT_PAIRED, CON_PAIRING_ACTIVE или CON_PAIRED
 */
int Wireless_Pairing_Status_Get(void)
{
    if (pairing_active)
    {
        return CON_PAIRING_ACTIVE;
    }

    uint8_t peer[6];
    S_MC_Get_Paired_Display_id(peer);
    bool has_any_nonzero = false;
    for (int i = 0; i < 6; i++)
    {
        if (peer[i] != 0)
        {
            has_any_nonzero = true;
            break;
        }
    }
    if (!has_any_nonzero)
    {
        logI("Pairing not found, mac: %02x:%02x:%02x:%02x:%02x:%02x",
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
        return CON_NOT_PAIRED;
    }
    logI("Pairing found, mac: %02x:%02x:%02x:%02x:%02x:%02x",
         peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
    return CON_PAIRED;
}

/**
 * @brief Начать процедуру привязки (pairing).
 * Запускается задача, которая периодически рассылает пакет со своим MAC-адресом.
 */
void Wireless_Pairing_Begin(void)
{
    // Сбрасываем все внутренние флаги
    pairing_active      = true;
    have_temp_peer      = false;
    got_done_from_peer  = false;
    memset(temp_peer_mac, 0, sizeof(temp_peer_mac));

    // Сбрасываем сохранённый ранее MAC (чтобы избежать «частичной» прошлой привязки)
    uint8_t zero_mac[6] = {0};
    S_MC_Set_Paired_Display_id(zero_mac);

    // Регистрируем обработчик приёма системных сообщений
    Wireless_Channel_Receive_Callback_Register(wireless_pairing_receive_cb, W_CHAN_SYSTEM);

    // Создаём задачу на привязку
    xTaskCreate(wireless_pairing_task, "W_Pair_Tsk", 4096, NULL, 2, NULL);
}

/*------------------------------------------------------------------------------
 * Локальные (static) функции
 *----------------------------------------------------------------------------*/

/**
 * @brief Задача привязки.
 *
 * Периодически рассылает W_MSG_TYPE_SYSTEM_PAIRING_MAC со своим MAC в поле peer_addr.
 * Завершается либо при успехе (взаимное подтверждение), либо по таймауту.
 */
static void wireless_pairing_task(void *arg)
{
    const TickType_t begin   = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(10000); // Тайм-аут привязки (10 секунд)

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    logI("Pairing task started, local MAC: %02x:%02x:%02x:%02x:%02x:%02x",
         my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);

    while ((xTaskGetTickCount() - begin) < timeout)
    {
        // Формируем системное сообщение на отправку
        w_header_sys_t *p_msg = (w_header_sys_t *)calloc(1, sizeof(w_header_sys_t));
        p_msg->message_type = W_MSG_TYPE_SYSTEM_PAIRING_MAC;
        memcpy(p_msg->peer_addr, my_mac, 6);
        p_msg->channel = 0; // Не используется

        // Отправляем блок (дальнейшее освобождение памяти внутри Rdt_SendBlock)
        bool sent = Rdt_SendBlock(W_CHAN_SYSTEM, (void *)p_msg, sizeof(*p_msg), NULL);
        if (sent)
        {
            // Если не удалось отправить, освободим память самостоятельно
            //free(p_msg);
            logE("Failed to send pairing request block");
        }
        else
        {
            logI("Broadcasting pairing request...");
        }

        // Проверяем, не было ли получено подтверждение «DONE» от пира
        if (got_done_from_peer)
        {
            // Другой узел подтвердил, что нас сохранил => финализация
            finalize_pairing();
            vTaskDelete(NULL);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Если мы здесь, значит вышли по таймауту
    logW("Pairing task timed out");
    revert_pairing();
    vTaskDelete(NULL);
}

/**
 * @brief Callback для обработки входящих пакетов в канале W_CHAN_SYSTEM во время привязки.
 *
 * Содержит логику обработки двух типов сообщений:
 * - W_MSG_TYPE_SYSTEM_PAIRING_MAC: сохранение MAC отправителя и отправка "DONE"
 * - W_MSG_TYPE_SYSTEM_PAIRING_DONE: отметка, что peer тоже сохранил наш MAC
 *
 */
static void wireless_pairing_receive_cb(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if (!pairing_active)
    {
        // Если привязка уже не активна, игнорируем
        return;
    }

    if (id != W_CHAN_SYSTEM)
    {
        logE("Invalid event ID: %"PRId32"", id);
        return;
    }

    // Извлекаем блок данных
    rdt_block_item_t block_item;
    if (!Rdt_ReceiveBlock(W_CHAN_SYSTEM, &block_item, 0))
    {
        // Не получилось достать блок
        logE("Failed to receive block from W_CHAN_SYSTEM");
        return;
    }

    if (block_item.data_size != sizeof(w_header_sys_t))
    {
        logE("Invalid block size: %zu, expected %zu",
             block_item.data_size, sizeof(w_header_sys_t));
        Rdt_FreeReceivedBlock(&block_item);
        return;
    }

    w_header_sys_t *p_msg = (w_header_sys_t *)block_item.data_ptr;

    switch (p_msg->message_type)
    {
    case W_MSG_TYPE_SYSTEM_PAIRING_MAC:
    {
        logI("Received PAIRING_MAC from %02x:%02x:%02x:%02x:%02x:%02x",
             p_msg->peer_addr[0], p_msg->peer_addr[1], p_msg->peer_addr[2],
             p_msg->peer_addr[3], p_msg->peer_addr[4], p_msg->peer_addr[5]);

        bool is_zero_mac = true;
        for (int i = 0; i < 6; i++)
        {
            if (p_msg->peer_addr[i] != 0)
            {
                is_zero_mac = false;
                break;
            }
        }
        if (is_zero_mac)
        {
            logW("Pairing request with zero MAC - ignoring");
            Rdt_FreeReceivedBlock(&block_item);
            return;
        }

        // Сохраняем MAC пира в локальный буфер, если ещё не сохранили
        // или если это совпадает с уже сохранённым адресом.
        if (!have_temp_peer)
        {
            memcpy(temp_peer_mac, p_msg->peer_addr, 6);
            have_temp_peer = true;
        }
        else
        {
            // Если уже что-то сохранено, проверим, совпадает ли
            // Если другой MAC — можно логировать, но обычно предполагается 1:1
            bool same_peer = (memcmp(temp_peer_mac, p_msg->peer_addr, 6) == 0);
            if (!same_peer)
            {
                logW("Received pairing request from different peer, ignoring");
                // Но не сбрасываем текущее состояние, 
                // т.к. уже начат процесс с другим устройством
            }
        }

        // Отправляем подтверждение (DONE) — «Я принял твой MAC»
        {
            // Формируем ответ
            uint8_t my_mac[6];
            esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

            w_header_sys_t *p_done = (w_header_sys_t *)calloc(1, sizeof(w_header_sys_t));
            p_done->message_type = W_MSG_TYPE_SYSTEM_PAIRING_DONE;
            memcpy(p_done->peer_addr, my_mac, 6);
            p_done->channel = 0;

            bool sent = Rdt_SendBlock(W_CHAN_SYSTEM, (void *)p_done, sizeof(*p_done), NULL);
            if (sent)
            {
                //free(p_done);
                logE("Failed to send DONE packet");
            }
            else
            {
                logI("Sending DONE packet in response to pairing request");
            }
        }
        break;
    }

    case W_MSG_TYPE_SYSTEM_PAIRING_DONE:
    {
        logI("Received PAIRING_DONE from %02x:%02x:%02x:%02x:%02x:%02x",
             p_msg->peer_addr[0], p_msg->peer_addr[1], p_msg->peer_addr[2],
             p_msg->peer_addr[3], p_msg->peer_addr[4], p_msg->peer_addr[5]);

        bool is_zero_mac = true;
        for (int i = 0; i < 6; i++)
        {
            if (p_msg->peer_addr[i] != 0)
            {
                is_zero_mac = false;
                break;
            }
        }
        if (is_zero_mac)
        {
            logW("Received DONE from zero MAC - ignoring");
            Rdt_FreeReceivedBlock(&block_item);
            return;
        }

        // По факту получения DONE мы убеждаемся, что peer нас тоже сохранил
        // Сохраняем MAC (если ещё не сохраняли) и выставляем флаг
        if (!have_temp_peer)
        {
            memcpy(temp_peer_mac, p_msg->peer_addr, 6);
            have_temp_peer = true;
        }
        else
        {
            // Аналогичная проверка, можно ли переписывать
            // Предполагается, что MAC совпадает с тем, что мы видели
            // Но если нет — логируем
            bool same_peer = (memcmp(temp_peer_mac, p_msg->peer_addr, 6) == 0);
            if (!same_peer)
            {
                logW("Received DONE from a different peer than expected");
                // Решение: пока игнорируем, оставляя текущего
            }
        }

        // Установка флага: «другая сторона подтвердила, что сохранила наш MAC»
        got_done_from_peer = true;
        break;
    }

    default:
        logW("Received unknown system message type %d", p_msg->message_type);
        break;
    }

    // Не забываем освобождать память
    Rdt_FreeReceivedBlock(&block_item);
}

/**
 * @brief Финализировать успешную привязку.
 *
 * Сюда попадаем, если обмен завершён успешно: есть MAC пира, и мы получили подтверждение, 
 * что peer тоже нас сохранил.
 */
static void finalize_pairing(void)
{
    logI("Pairing successful, finalizing...");

    // Сохраняем MAC в постоянной памяти
    S_MC_Set_Paired_Display_id(temp_peer_mac);
    S_Commit_All();

    // Добавляем peer в ESP-NOW (если это требуется)
    Rdt_AddPeer(temp_peer_mac);

    // Снимаем флаг активности
    pairing_active = false;

    // Отзываем коллбек, т.к. процедура привязки завершена
    Wireless_Channel_Receive_Callback_Unregister(wireless_pairing_receive_cb, W_CHAN_SYSTEM);
}

/**
 * @brief Откат (revert) привязки при таймауте или ошибке.
 */
static void revert_pairing(void)
{
    logW("Reverting pairing — no mutual confirmation");

    // Полностью обнуляем сохранённый MAC, если он был 
    uint8_t zero_mac[6] = {0};
    S_MC_Set_Paired_Display_id(zero_mac);

    // Снимаем флаг активности
    pairing_active = false;

    // Отзываем коллбек
    Wireless_Channel_Receive_Callback_Unregister(wireless_pairing_receive_cb, W_CHAN_SYSTEM);

    // Сбрасываем внутренние переменные
    have_temp_peer     = false;
    got_done_from_peer = false;
    memset(temp_peer_mac, 0, sizeof(temp_peer_mac));
}
