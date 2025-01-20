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

void Wireless_Channels_Init(void);
void Wireless_Channel_Receive_Callback_Register(esp_event_handler_t cb, int channel);
int Wireless_Pairing_Status_Get(void);
void Wireless_Pairing_Begin(void);

#endif