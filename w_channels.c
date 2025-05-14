/**
 * @file w_connect.c
 * @author Pavel
 * @brief Инициализация каналов
 * @date 2025-01-20
 * 
 */

#include "w_main.h"
#include "w_user.h"
#include "wireless_port.h"
#define TAG "w_channels"
#include "log.h"

//ESP_EVENT_DEFINE_BASE(WIRELESS_EVENT_BASE);

// Список callbacks
//static void (*Wireless_Channel_Receive_Callbacks[RDT_MAX_CHANNELS])();

void Wireless_Channels_Init(void) 
{
    int ret = 0;
    ret = Rdt_ChannelInit(W_CHAN_SENSORS, W_CHAN_SENSORS_RECEIVE_QUEUE_SIZE, W_CHAN_SENSORS_SEND_QUEUE_SIZE, 512);
    if(ret != 0)
    {
        logE("Rdt_ChannelInit failed");
    }
    ret = Rdt_ChannelInit(W_CHAN_SYSTEM, 5, 5, 512);
    if(ret != 0)
    {
        logE("Rdt_ChannelInit failed");
    }
    ret = Rdt_ChannelInit(W_CHAN_PARAMS, 5, 5, 512);
    if(ret != 0)
    {
        logE("Rdt_ChannelInit failed");
    }
    ret = Rdt_ChannelInit(W_CHAN_FILES, 5, 5, 512);
    if(ret != 0)
    {
        logE("Rdt_ChannelInit failed");
    }
}

void Wireless_Channel_Receive_Callback_Register(esp_event_handler_t cb, int channel)
{
    if (channel < 0 || channel >= RDT_MAX_CHANNELS)
    {
        logE("Invalid channel: %d", channel);
        return;
    }

    if (cb == NULL)
    {
        logE("Callback is NULL for channel: %d", channel);
        return;
    }
    
    Wireless_Channel_Clear_Queue(channel);

    esp_err_t err = esp_event_handler_register_with(W_event_loop, WIRELESS_EVENT_BASE, channel, cb, NULL);
    if (err == ESP_OK)
    {
        logI("Callback registered for channel %d", channel);
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        logE("Invalid arguments for event handler registration");
    }
    else if (err == ESP_ERR_NO_MEM)
    {
        logE("Not enough memory to register callback");
    }
    else
    {
        logE("Unknown error: %d", err);
    }
}

void Wireless_Channel_Receive_Callback_Unregister(esp_event_handler_t cb, int channel)
{
    if (channel < 0 || channel >= RDT_MAX_CHANNELS)
    {
        logE("Invalid channel: %d", channel);
        return;
    }
    
    esp_err_t err = esp_event_handler_unregister_with(W_event_loop, WIRELESS_EVENT_BASE, channel, cb);
    if (err == ESP_OK)
    {
        logI("Callback unregistered for channel %d", channel);
    }
    else if (err == ESP_ERR_INVALID_ARG)
    {
        logE("Invalid arguments for event handler unregistration");
    }
    else if (err == ESP_ERR_NOT_FOUND)
    {
        logE("Callback not found for channel %d", channel);
    }
    else
    {
        logE("Unknown error: %d", err);
    }
}