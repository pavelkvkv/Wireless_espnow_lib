/**
 * @file w_connect.c
 * @author Pavel
 * @brief Функции для высокоуровневого использования библиотеки wireless_lib
 * @date 2025-01-20
 * 
 */

#ifndef _W_USER_H_
#define _W_USER_H_

#include "w_main.h"



/**
 * Каналы для обмена данными
 */
enum
{
    W_CHAN_SYSTEM,      // Сообщения пейринга
    W_CHAN_SENSORS,     // Контакты и реле МС
    W_CHAN_PARAMS,      // Чтение-запись параметров
    W_CHAN_FILES,       // Чтение-запись файлов
};

enum
{
    W_MSG_TYPE_SYSTEM_PAIRING_MAC   = 1,
    W_MSG_TYPE_SYSTEM_PAIRING_DONE  = 2,

    W_MSG_TYPE_SENSORS_IO       = 10,
    W_MSG_TYPE_SENSORS_RELAY    = 11,
    W_MSG_TYPE_SENSORS_THERMO   = 12,

    W_MSG_TYPE_PARAM_TIME       = 20,
    W_MSG_TYPE_PARAM_MC_CONFIG  = 21,
    //W_MSG_TYPE_PARAM_MC_TITLES  = 22,
};

enum 
{
    W_PARAM_GET = 0,
    W_PARAM_SET = 1
};

typedef struct
{
    uint8_t message_type;     // Тип сообщения
    uint8_t peer_addr[6];     // MAC-адрес отправителя
    uint8_t channel;          // Доп. поле (не используется в данном примере)
} w_header_sys_t;

typedef struct
{
    u8 message_type;
    u8 data[0];
} w_header_sensors_t;

typedef struct
{
    u8 message_type;
    u8 set_or_get;
    u8 data[0];
} w_header_param_t;


void Wireless_Channels_Init(void);
void Wireless_Channel_Receive_Callback_Register(esp_event_handler_t cb, int channel);
void Wireless_Channel_Receive_Callback_Unregister(esp_event_handler_t cb, int channel);
int Wireless_Pairing_Status_Get(void);
void Wireless_Pairing_Begin(void);

#endif