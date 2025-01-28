#ifndef W_FILES_H
#define W_FILES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Максимальная длина пути к файлу (в байтах)
 */
#define W_FILES_MAX_PATH   128

/**
 * @brief Максимальный размер передаваемых данных (в байтах) за один запрос
 */
#define W_FILES_MAX_DATA   (4*1024) 

/* ----------------------------------------------------------------
 * Структура заголовка (протокола) для запросов/ответов
 * ----------------------------------------------------------------
 * Упакованные поля (Little Endian), для простоты:
 *  - command: код команды (W_FILES_CMD_XXX)
 *  - return_code: код результата (заполняется в ответе)
 *  - request_id: короткий ID запроса, чтобы сопоставить ответ
 *  - offset: смещение файла (для READ/WRITE). Если == (uint32_t)-1 => "append"
 *  - data_length: кол-во байт данных, следующих за заголовком
 *  - path_length: длина пути (далее идут path_length байт пути)
 *
 * Затем идут path_length байт с путём,
 * затем data_length байт с данными (если есть, напр. при WRITE).
 */
#pragma pack(push, 1)
typedef struct
{
	uint8_t command;
	uint8_t return_code;
	uint16_t request_id;
	uint32_t offset;
	uint32_t data_length;
	uint8_t path_length;
	uint8_t reserved[3]; // на будущее, для выравнивания
	// Дальше в памяти: path[], data[]
} w_files_header_t;
#pragma pack(pop)

// Для удобства ограничим максимальный общий размер пакета (заголовок + путь + данные)
#define W_FILES_MAX_PACKET_SIZE (sizeof(w_files_header_t) + W_FILES_MAX_PATH + W_FILES_MAX_DATA)

/**
 * @brief Коды команд для нашего протокола
 */
enum {
    W_FILES_CMD_LIST        = 1,  ///< Запрос списка файлов в каталоге
    W_FILES_CMD_LIST_RESP   = 2,  ///< Ответ со списком

    W_FILES_CMD_READ        = 3,  ///< Запрос чтения
    W_FILES_CMD_READ_RESP   = 4,  ///< Ответ с данными

    W_FILES_CMD_WRITE       = 5,  ///< Запрос записи
    W_FILES_CMD_WRITE_RESP  = 6,  ///< Ответ о результате записи
};

/**
 * @brief Коды возврата (return_code) в ответах
 */
enum {
    W_FILES_OK             = 0, ///< Успешно
    W_FILES_ERR_UNKNOWN    = 1, ///< Неизвестная команда
    W_FILES_ERR_NOFILE     = 2, ///< Файл не найден / ошибка открытия
    W_FILES_ERR_IO         = 3, ///< Ошибка чтения/записи
    W_FILES_ERR_TOOLARGE   = 4, ///< Превышен лимит (длина пути или объём данных)
    W_FILES_ERR_INTERNAL   = 5, ///< Другая внутренняя ошибка
    // ... при необходимости можно расширять
};



/**
 * @brief Деинициализация модуля (опционально).
 * Можно вызвать при завершении работы, чтобы освободить ресурсы.
 */
void w_files_deinit(void);

/**
 * @brief Получение списка файлов (и их размеров) в указанном каталоге (блокирующий вызов).
 * 
 * @param[in]  directory     Путь к каталогу (null-terminated)
 * @param[out] out_data      Буфер, куда будет записан список (текстом или иной схемой)
 * @param[in,out] inout_size На входе: размер буфера. На выходе: реальный размер записанных данных
 * @param[in]  wait_ticks    Таймаут (в тиках) ожидания ответа
 * @param[out] return_code   Код возврата из ответа (W_FILES_OK = 0 при успехе)
 * 
 * @return 0 при успешном завершении (ответ получен), !=0 при ошибке или таймауте.
 */
int w_files_list(const char *directory,
                 uint8_t *out_data,
                 size_t *inout_size,
                 TickType_t wait_ticks,
                 uint8_t *return_code);

/**
 * @brief Чтение сегмента файла (блокирующий вызов).
 * 
 * @param[in]  path          Путь к файлу
 * @param[in]  offset        Смещение (байты от начала файла)
 * @param[out] out_data      Буфер для чтения
 * @param[in,out] inout_size На входе: размер буфера. На выходе: фактически считанные байты
 * @param[in]  wait_ticks    Таймаут ожидания
 * @param[out] return_code   Код возврата из ответа
 * 
 * @return 0 при успешном завершении запроса, иначе ошибка/таймаут.
 */
int w_files_read(const char *path,
                 size_t offset,
                 uint8_t *out_data,
                 size_t *inout_size,
                 TickType_t wait_ticks,
                 uint8_t *return_code);

/**
 * @brief Запись сегмента файла (блокирующий вызов).
 * 
 * Если offset == (size_t)-1, считается, что нужно «дописать в конец».
 * Иначе запись идёт по указанному смещению.
 * 
 * @param[in]  path          Путь к файлу
 * @param[in]  offset        Смещение или (size_t)-1 для записи в конец
 * @param[in]  data          Данные для записи
 * @param[in]  data_len      Число байт для записи
 * @param[in]  wait_ticks    Таймаут ожидания
 * @param[out] return_code   Код возврата из ответа
 * 
 * @return 0 при успешном выполнении запроса, !=0 при ошибке/таймауте
 */
int w_files_write(const char *path,
                  size_t offset,
                  const uint8_t *data,
                  size_t data_len,
                  TickType_t wait_ticks,
                  uint8_t *return_code);

#ifdef __cplusplus
}
#endif

#endif // W_FILES_H
