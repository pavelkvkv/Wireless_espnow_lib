#ifndef W_MAIN_H
#define W_MAIN_H

#define IPADDR_HOST_AP "192.168.109.1"
#define IPADDR_HOST_STA "192.168.109.2"

enum
{
    PORT_SENSORS =  1000,   // Рассылка данных с датчиков
    PORT_PAIRING =  1001,   // Запрос-ответ для привязки
    PORT_PARAMS =   1002,   // Запрос-ответ параметров
    PORT_FILES =    1003,   // Запрос-ответ с передачей файлов
    PORT_STATUSES = 1004,   // Рассылка статусов
};

enum 
{
    //WIRELESS_NOT_PAIRED =   (1 << 0),
    WIRELESS_PAIRED =               (1 << 1),
    WIRELESS_CONNECTED =            (1 << 2),
    WIRELESS_PAIRING_IN_PROCESS =   (1 << 3),
    WIRELESS_PAIR_FAILED =          (1 << 4),
};



void Wireless_Init();
void Wireless_Update_Config();
int Wireless_Connect_Status_Get();
void Wireless_Pairing_Begin();

#endif // W_MAIN_H  