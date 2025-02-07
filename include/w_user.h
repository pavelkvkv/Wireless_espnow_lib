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
    /* Системные сообщения */
    W_MSG_TYPE_SYSTEM_PAIRING_MAC   = 1,    // Начальное сообщение привязки
    W_MSG_TYPE_SYSTEM_PAIRING_DONE  = 2,    // Завершение привязки

    /* Рассылка сенсоров */
    W_MSG_TYPE_SENSORS_IO       = 10,       // Рассылка данных сухих контактов
    W_MSG_TYPE_SENSORS_RELAY    = 11,       // Рассылка данных реле
    W_MSG_TYPE_SENSORS_THERMO   = 12,       // Рассылка данных термометров

    /* Чтение-запись параметров */
    W_MSG_TYPE_PARAM_TIME               = 20,    // Время (time_t)
    W_MSG_TYPE_PARAM_MC_CONFIG          = 21,    // Конфигурация МС (StructMC_Config_t)
    W_MSG_TYPE_PARAM_MC_TITLES_IO       = 22,    // Названия контактов
    W_MSG_TYPE_PARAM_MC_TITLES_RELAY    = 23,    // Названия реле
    W_MSG_TYPE_PARAM_MC_TITLES_THERMO   = 24,    // Названия термометров
    W_MSG_TYPE_PARAM_DISP_FWVER         = 25,    // Версия прошивки дисплея
    W_MSG_TYPE_PARAM_RULES              = 26,    // Правила MC
    W_MSG_TYPE_PARAM_DIRECT_RELAY       = 27,    // Параметры прямого управления реле
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
    u8 data[];
} w_header_sensors_t;



void Wireless_Channels_Init(void);
void Wireless_Channel_Receive_Callback_Register(esp_event_handler_t cb, int channel);
void Wireless_Channel_Receive_Callback_Unregister(esp_event_handler_t cb, int channel);
int Wireless_Pairing_Status_Get(void);
void Wireless_Pairing_Begin(void);


#endif