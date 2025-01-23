#ifndef W_PARAM_H
#define W_PARAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PARAM_LENGTH (1024*8) ///< Максимальная длина параметра

/**
 * @brief Типы операций с параметрами и ответов
 */
enum {
    W_PARAM_GET  = 0,  ///< Запрос: чтение параметра
    W_PARAM_SET  = 1,  ///< Запрос: запись параметра
    W_PARAM_RESP = 2   ///< Ответ: результат выполнения GET/SET
};

/**
 * @brief Заголовок пакета параметров
 *
 * Поля:
 *  - message_type: тип параметра (например, W_MSG_TYPE_PARAM_XXX)
 *  - set_or_get: тип операции (W_PARAM_GET, W_PARAM_SET или W_PARAM_RESP)
 *  - return_code: код возврата (0 = успех, !=0 = ошибка)
 *  - data: полезная нагрузка (данные параметра)
 */
#pragma pack(push, 1)
typedef struct
{
    uint8_t message_type;  ///< Тип параметра
    uint8_t set_or_get;    ///< Тип операции (GET, SET или RESP)
    uint8_t return_code;   ///< Код возврата
    uint8_t data[0];       ///< Полезная нагрузка (данные)
} w_header_param_t;
#pragma pack(pop)

/**
 * @brief Тип функции чтения параметра
 * @param[out] out_data  Буфер для записи значения параметра
 * @param[in,out] out_size
 *    - Вход: размер доступного буфера
 *    - Выход: фактический размер записанных данных
 * @return Код возврата (0 = успех, !=0 = ошибка)
 */
typedef int (*w_param_read_fn)(uint8_t *out_data, size_t *out_size);

/**
 * @brief Тип функции записи параметра
 * @param[in] in_data  Указатель на новые данные параметра
 * @param[in] in_size  Размер данных параметра
 * @return Код возврата (0 = успех, !=0 = ошибка)
 */
typedef int (*w_param_write_fn)(const uint8_t *in_data, size_t in_size);

/**
 * @brief Дескриптор параметра в реестре
 * @details
 *  - message_type: соответствует W_MSG_TYPE_PARAM_XXX.
 *  - read_fn и write_fn: функции чтения и записи (могут быть NULL, если операция не поддерживается).
 */
typedef struct
{
    uint8_t             message_type; ///< Тип параметра (например, W_MSG_TYPE_PARAM_TIME)
    w_param_read_fn     read_fn;      ///< Функция чтения параметра (NULL, если не поддерживается)
    w_param_write_fn    write_fn;     ///< Функция записи параметра (NULL, если не поддерживается)
} w_param_descriptor_t;

/**
 * @brief Инициализация модуля параметров
 * @param[in] table        Указатель на таблицу дескрипторов параметров
 * @param[in] table_count  Количество элементов в таблице
 *
 * Эта функция должна быть вызвана перед началом работы модуля.
 */
void w_param_init(const w_param_descriptor_t *table, size_t table_count);

/**
 * @brief Деинициализация модуля параметров
 *
 * Опционально: завершение работы модуля и освобождение ресурсов.
 */
void w_param_deinit(void);

/**
 * @brief Запуск механизма обработки параметров
 *
 * Регистрирует callback для обработки входящих запросов (GET/SET).
 */
void w_param_start(void);

/**
 * @brief Блокирующий запрос GET или SET с ожиданием ответа
 * @param[in]  message_type  Тип параметра (W_MSG_TYPE_PARAM_XXX)
 * @param[in]  set_or_get    W_PARAM_GET или W_PARAM_SET
 * @param[in]  value         Данные для SET (NULL для GET)
 * @param[in]  value_len     Размер данных для SET (0 для GET)
 * @param[out] resp_data     Буфер для получения ответа (NULL, если не нужен)
 * @param[in,out] resp_size  Размер буфера на входе, фактический размер на выходе
 * @param[in]  wait_ticks    Таймаут ожидания ответа (например, pdMS_TO_TICKS(1000))
 * @param[out] return_code   Код возврата из ответа (0 = успех, !=0 = ошибка)
 *
 * @return 0, если запрос выполнен успешно (ответ получен), !=0 в случае ошибки или таймаута.
 */
int w_param_request_blocking(uint8_t message_type,
                             uint8_t set_or_get,
                             const uint8_t *value,
                             size_t value_len,
                             uint8_t *resp_data,
                             size_t *resp_size,
                             TickType_t wait_ticks,
                             uint8_t *return_code);

/**
 * @brief Асинхронный запрос GET или SET (без ожидания ответа)
 * @param[in] message_type
 * @param[in] set_or_get
 * @param[in] value
 * @param[in] value_len
 * @return 0 при успешной отправке, !=0 в случае ошибки
 */
int w_param_send_request_async(uint8_t message_type,
                               uint8_t set_or_get,
                               const uint8_t *value,
                               size_t value_len);

/**
 * @brief Стандартный таймаут для запросов параметров (например, 2000 мс)
 */
#define W_PARAM_DEFAULT_TIMEOUT pdMS_TO_TICKS(2000)

/**
 * @brief Обёртка для выполнения GET-запроса с использованием стандартного таймаута
 * @param[in]  message_type  Тип параметра (W_MSG_TYPE_PARAM_XXX)
 * @param[out] resp_data     Буфер для полученных данных
 * @param[in,out] resp_size  Размер буфера на входе, фактический размер на выходе
 * @param[out] return_code   Код возврата из ответа (0 = успех, !=0 = ошибка)
 *
 * @return 0 при успешном выполнении запроса, !=0 в случае ошибки
 */
int w_param_get(uint8_t message_type,
                uint8_t *resp_data,
                size_t *resp_size,
                uint8_t *return_code);

/**
 * @brief Обёртка для выполнения SET-запроса с использованием стандартного таймаута
 * @param[in]  message_type  Тип параметра (W_MSG_TYPE_PARAM_XXX)
 * @param[in]  value         Данные для установки параметра
 * @param[in]  value_len     Размер данных параметра
 * @param[out] return_code   Код возврата из ответа (0 = успех, !=0 = ошибка)
 *
 * @return 0 при успешном выполнении запроса, !=0 в случае ошибки
 */
int w_param_set(uint8_t message_type,
                const uint8_t *value,
                size_t value_len,
                uint8_t *return_code);

#ifdef __cplusplus
}
#endif

#endif // W_PARAM_H
