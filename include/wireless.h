/**
 * @file wireless.h
 * @brief Высокоуровневые структурки для передачи через библиотеку
 * @date 2025-01-21
 * @author Pavel
 */

#ifndef _WIRELESS_H_
#define _WIRELESS_H_

#include "w_user.h"


enum
{
    W_MSG_TYPE_SYSTEM_PAIRING   = 1,

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
    u8 message_type;
    u8 peer_addr[6];
    u8 channel;
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

#endif  /* _WIRELESS_H_ */