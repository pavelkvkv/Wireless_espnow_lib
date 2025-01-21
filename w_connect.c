/**
 * @file w_connect.c
 * @author Pavel
 * @brief Пейринг двух устройств по espnow
 * @date 2025-01-20
 * 
 */

#include "w_main.h"
#include "w_user.h"
#include "wireless_port.h"
#include "esp_event.h"
#include "esp_mac.h"

#define TAG "w_connect"
#include "../../main/include/log.h"

static void wireless_pairing_receive_cb(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data);
static void wireless_pairing_task(void *arg);

static bool pairing_active = false;

/** @brief Получить статус привязки */
int Wireless_Pairing_Status_Get(void) 
{
    if(pairing_active) return CON_PAIRING_ACTIVE;
    uint8_t peer[6] = {0};
    S_MC_Get_Paired_Display_id(peer);
    if (peer[0] == 0) 
    {
        return CON_NOT_PAIRED;
    }
    return CON_PAIRED;
}

/** @brief Начало привязки */
void Wireless_Pairing_Begin(void)
{
    pairing_active = true;
    uint8_t peer[6] = {0};
    S_MC_Set_Paired_Display_id(peer); // Очистить сохранённый MAC
    Wireless_Channel_Receive_Callback_Register(wireless_pairing_receive_cb, W_CHAN_SYSTEM);

    xTaskCreate(wireless_pairing_task, "W_Pair_Tsk", 4096, NULL, 2, NULL);
}

/** @brief Задача привязки */
static void wireless_pairing_task(void *arg)
{
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(10000); // Тайм-аут привязки (10 секунд)
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    logI("Pairing task started, MAC: %02x:%02x:%02x:%02x:%02x:%02x",
         my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);

    while ((xTaskGetTickCount() - begin) < timeout)
    {
        Rdt_SendBlock(W_CHAN_SYSTEM, my_mac, 6, NULL);
        logI("Broadcasting pairing request...");
        vTaskDelay(pdMS_TO_TICKS(500)); // Отправлять каждые 500 мс

        // Проверяем статус привязки
        if (Wireless_Pairing_Status_Get() == CON_PAIRED)
        {
            logI("Pairing successful");
            pairing_active = false;
            vTaskDelete(NULL); // Завершаем задачу
        }
    }

    logW("Pairing task timed out");
    pairing_active = false;
    vTaskDelete(NULL); // Тайм-аут, завершаем задачу
}

/** @brief Обратный вызов для обработки пакетов привязки */
static void wireless_pairing_receive_cb(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if (id != W_CHAN_SYSTEM)
    {
        logE("Invalid event ID: %"PRId32"", id);
        return;
    }

    uint8_t peer[6] = {0};
    rdt_block_item_t block_item;

    // Извлекаем данные
    if (!Rdt_ReceiveBlock(W_CHAN_SYSTEM, &block_item, 0))
    {
        logE("Failed to receive block");
        return;
    }

    if (block_item.data_size != 6) // Ожидаем MAC-адрес (6 байт)
    {
        logE("Invalid pairing block size: %zu", block_item.data_size);
        Rdt_FreeReceivedBlock(&block_item);
        return;
    }

    // Копируем MAC-адрес
    memcpy(peer, block_item.data_ptr, 6);
    Rdt_FreeReceivedBlock(&block_item);

    // Проверяем MAC-адрес
    if (peer[0] == 0)
    {
        logW("Pairing with zero MAC address failed");
        return;
    }

    logI("Pairing with %02x:%02x:%02x:%02x:%02x:%02x",
         peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);

    // Сохраняем привязанный MAC-адрес
    S_MC_Set_Paired_Display_id(peer);
}