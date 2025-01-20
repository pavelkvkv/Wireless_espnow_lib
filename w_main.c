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


#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "string.h"

#include "w_main.h"
#include "wireless_port.h"

#define TAG "wireless_main"
#include "../../main/include/log.h"

#define ESSID_LEN               7
#define PASS_LEN                15
#define DEFAULT_CHANNEL         1
#define DEFAULT_SPEED           WIFI_PHY_RATE_LORA_250K
#define DEFAULT_WLAN_PROTO      WIFI_PROTOCOL_LR
//#define MAX_RETRY 999
#define TIMEOUT_FIRST_REQUEST   30
#define TIMEOUT_OTHER           3


static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;

static esp_netif_t *wifi_netif;
static int pairing_status = 0;
static TaskHandle_t pairing_task_handle = NULL;
static bool initialized = false;
const static wifi_protocols_t proto = {.ghz_2g = DEFAULT_WLAN_PROTO};

extern void S_MC_Get_Paired_Display_id(u8 * mac);
extern void S_MC_Set_Paired_Display_id(const u8 * mac);







