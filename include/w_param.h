#ifndef W_PARAM_H
#define W_PARAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PARAM_LENGHT 1024*8
/**
 * @brief Типы операций с параметром
 */
enum {
    W_PARAM_GET = 0,
    W_PARAM_SET = 1
};

/**
 * @brief Структура заголовка для передачи параметров
 * 
 * Принцип:
 *  - message_type: один из W_MSG_TYPE_PARAM_XXX
 *  - set_or_get: 0 (GET) или 1 (SET)
 *  - return_code: код возврата из функции чтения/записи (0 - успех, !=0 - ошибка)
 *  - data: тело параметра (в случае GET-ответа — значение параметра; в случае SET-запроса — новые данные)
 */
#pragma pack(push, 1)
typedef struct
{
    uint8_t message_type;  
    uint8_t set_or_get;    
    uint8_t return_code;   
    uint8_t data[0];       
} w_header_param_t;
#pragma pack(pop)

/**
 * @brief Тип функции чтения параметра
 * @param[out] out_data  Буфер для записи значения
 * @param[in,out] out_size На входе содержит размер доступного буфера out_data,
 *                         на выходе функция должна заполнить фактический размер записанных данных
 * @return Код возврата (0 - успех, !=0 - ошибка)
 */
typedef int (*w_param_read_fn)(uint8_t *out_data, size_t *out_size);

/**
 * @brief Тип функции записи параметра
 * @param[in] in_data  Указатель на входные данные (новое значение параметра)
 * @param[in] in_size  Размер входных данных
 * @return Код возврата (0 - успех, !=0 - ошибка)
 */
typedef int (*w_param_write_fn)(const uint8_t *in_data, size_t in_size);

/**
 * @brief Описание одного параметра в реестре
 * @details
 *  - message_type соответствует W_MSG_TYPE_PARAM_XXX.
 *  - read_fn и write_fn — функции чтения и записи (любые из них могут быть NULL).
 */
typedef struct
{
    uint8_t             message_type; ///< Например, W_MSG_TYPE_PARAM_TIME
    w_param_read_fn     read_fn;      ///< Функция чтения (NULL, если чтение не поддерживается)
    w_param_write_fn    write_fn;     ///< Функция записи (NULL, если запись не поддерживается)
} w_param_descriptor_t;

/**
 * @brief Инициализация параметров
 * @param[in] table        Указатель на таблицу параметров
 * @param[in] table_count  Количество элементов в таблице
 *
 * Данная функция должна быть вызвана до старта работы модуля,
 * чтобы зарегистрировать все параметры и функции чтения/записи.
 */
void w_param_init(const w_param_descriptor_t *table, size_t table_count);

/**
 * @brief Деинициализация (при необходимости)
 */
void w_param_deinit(void);

/**
 * @brief Запуск механизма приёма/обработки параметров (регистрация коллбека)
 *
 * Вызывается при старте модуля. Регистрирует коллбек на канал (или каналы),
 * в котором будут приходить запросы GET/SET параметров.
 */
void w_param_start(void);

/**
 * @brief Отправить запрос GET или SET
 * @param[in] message_type  Один из W_MSG_TYPE_PARAM_XXX
 * @param[in] set_or_get    0 = GET, 1 = SET
 * @param[in] value         Данные для SET (или NULL для GET)
 * @param[in] value_len     Размер данных для SET (или 0 для GET)
 * @return 0 - OK, !=0 - ошибка
 *
 * Пример использования: w_param_send_request(W_MSG_TYPE_PARAM_TIME, W_PARAM_GET, NULL, 0);
 */
int w_param_send_request(uint8_t message_type,
                         uint8_t set_or_get,
                         const uint8_t *value,
                         size_t value_len);

#ifdef __cplusplus
}
#endif

#endif // W_PARAM_H
