#include "w_files.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h> // Для чтения каталога (POSIX); адаптируйте под нужную среду

// Подключаем API Rdt и Wireless_*:
#include "w_main.h" // Rdt_SendBlock, Rdt_ReceiveBlock, Rdt_FreeReceivedBlock
#include "w_user.h"
#include "wireless_port.h" // Wireless_Channel_Receive_Callback_Register

#define TAG "w_files"
#include "../../main/include/log.h"

// Глобальные/статические переменные для блокирующей логики:
static bool g_initialized				= false;
static bool g_request_in_progress		= false;
static SemaphoreHandle_t g_mutex		= NULL;
static SemaphoreHandle_t g_response_sem = NULL;
static uint16_t g_next_request_id		= 1; // Чтобы уникально нумеровать запросы
static uint16_t g_current_request_id	= 0; // ID активного запроса

// Буферы для получения ответа
static uint8_t g_resp_return_code = 0xFF;
static uint8_t *g_resp_buffer	  = NULL; // сюда копируем данные из ответа
static size_t g_resp_data_len	  = 0;	  // фактический объём в g_resp_buffer

// Вспомогательные forward-декларации
static void w_files_receive_cb(void *handler_arg,
							   esp_event_base_t base,
							   int32_t id,
							   void *event_data);

static void w_files_handle_incoming_packet(const uint8_t *packet_data, size_t packet_size);
static void w_files_process_request(const w_files_header_t *hdr_in, size_t packet_size);
static void w_files_process_response(const w_files_header_t *hdr_in, size_t packet_size);

static int w_files_send_request_blocking(uint8_t command,
										 const char *path,
										 uint32_t offset,
										 const uint8_t *data,
										 size_t data_len,
										 uint8_t *out_data,
										 size_t *inout_size,
										 TickType_t wait_ticks,
										 uint8_t *return_code);

/**
 * Функции-врапперы из порта
 */
extern int w_port_filelist_get(const char *directory, uint8_t *resp_data, size_t *out_data_length);
extern FILE *w_port_fopen(const char *filename, const char *mode);
extern size_t w_port_fread(void *ptr, size_t size, size_t count, FILE *stream);
extern size_t w_port_fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
extern int w_port_fclose(FILE *stream);
extern int w_port_fseek(FILE *stream, long offset, int whence);
extern long w_port_ftell(FILE *stream);
extern void w_port_rewind(FILE *stream);

// ----------------------------------------------------------------
// Инициализация / Деинициализация
// ----------------------------------------------------------------

void Wireless_Files_Init(void)
{
	if (g_initialized)
	{
		return;
	}

	g_mutex				  = xSemaphoreCreateMutex();
	g_response_sem		  = xSemaphoreCreateBinary();
	g_request_in_progress = false;
	g_initialized		  = true;

	// Регистрируем колбэк приёма в канале W_CHAN_FILES
	Wireless_Channel_Receive_Callback_Register(w_files_receive_cb, W_CHAN_FILES);
}

void w_files_deinit(void)
{
	if (!g_initialized)
	{
		return;
	}
	g_initialized = false;

	// Отписываемся от колбэка (если нужно)
	Wireless_Channel_Receive_Callback_Unregister(w_files_receive_cb, W_CHAN_FILES);

	// Освобождаем ресурсы
	if (g_mutex)
	{
		vSemaphoreDelete(g_mutex);
		g_mutex = NULL;
	}
	if (g_response_sem)
	{
		vSemaphoreDelete(g_response_sem);
		g_response_sem = NULL;
	}
}

// ----------------------------------------------------------------
// Публичные функции API (блокирующие)
// ----------------------------------------------------------------

int w_files_list(const char *directory,
				 uint8_t *out_data,
				 size_t *inout_size,
				 TickType_t wait_ticks,
				 uint8_t *return_code)
{
	// Если directory == NULL, можно трактовать как ".", или вернуть ошибку
	if (!directory)
	{
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -1;
	}
	// Add last slash
	if (directory[strlen(directory) - 1] != '/') 
	{
		strcat(directory, "/");
	}
	return w_files_send_request_blocking(W_FILES_CMD_LIST,
										 directory,
										 0, // offset=0 (не важно для LIST)
										 NULL, 0,
										 out_data,
										 inout_size,
										 wait_ticks,
										 return_code);
}

int w_files_read(const char *path,
				 size_t offset,
				 uint8_t *out_data,
				 size_t *inout_size,
				 TickType_t wait_ticks,
				 uint8_t *return_code)
{
	if (!path)
	{
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -1;
	}
	return w_files_send_request_blocking(W_FILES_CMD_READ,
										 path,
										 (uint32_t)offset,
										 NULL, 0,
										 out_data,
										 inout_size,
										 wait_ticks,
										 return_code);
}

int w_files_write(const char *path,
				  size_t offset,
				  const uint8_t *data,
				  size_t data_len,
				  TickType_t wait_ticks,
				  uint8_t *return_code)
{
	if (!path)
	{
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -1;
	}
	// Если offset == (size_t)-1, трактуем как "дописать в конец" => offset=(uint32_t)-1
	uint32_t off32 = (offset == (size_t)-1) ? 0xFFFFFFFF : (uint32_t)offset;

	return w_files_send_request_blocking(W_FILES_CMD_WRITE,
										 path,
										 off32,
										 data,
										 data_len,
										 NULL, // чтения ответа, кроме return_code, не нужно
										 NULL,
										 wait_ticks,
										 return_code);
}

// ----------------------------------------------------------------
// Внутренние функции
// ----------------------------------------------------------------

/**
 * @brief Функция для формирования и отправки запроса, затем ожидания ответа (блокирующая).
 *
 * @param[in]  command     Команда (LIST/READ/WRITE)
 * @param[in]  path        Путь к файлу/каталогу
 * @param[in]  offset      Смещение (или 0xFFFFFFFF для "append")
 * @param[in]  data        Данные (для WRITE)
 * @param[in]  data_len    Размер данных для WRITE
 * @param[out] out_data    Буфер для приёма ответа (может быть NULL)
 * @param[in,out] inout_size Размер буфера / фактически принятых данных (может быть NULL)
 * @param[in]  wait_ticks  Таймаут ожидания ответа
 * @param[out] return_code Код возврата из ответа
 *
 * @return 0 при успешном получении ответа, иначе ошибка/таймаут.
 */
static int w_files_send_request_blocking(uint8_t command,
										 const char *path,
										 uint32_t offset,
										 const uint8_t *data,
										 size_t data_len,
										 uint8_t *out_data,
										 size_t *inout_size,
										 TickType_t wait_ticks,
										 uint8_t *return_code)
{
	if (!g_initialized)
	{
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -1;
	}

	// Блокируем мьютекс
	if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
	{
		// Не смогли взять мьютекс
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -2;
	}

	if (g_request_in_progress)
	{
		// Уже идёт запрос
		xSemaphoreGive(g_mutex);
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -3;
	}
	g_request_in_progress = true;

	// Сформируем пакет
	size_t path_len = strlen(path);
	if (path_len > W_FILES_MAX_PATH)
	{
		// Превышена максимально допустимая длина пути
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		if (return_code) *return_code = W_FILES_ERR_TOOLARGE;
		return -4;
	}
	if (data_len > W_FILES_MAX_DATA)
	{
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		if (return_code) *return_code = W_FILES_ERR_TOOLARGE;
		return -5;
	}

	// Увеличиваем счётчик request_id
	g_current_request_id = g_next_request_id++;
	if (g_next_request_id == 0)
	{
		g_next_request_id = 1; // не даём ему уйти в 0
	}

	// Подготовим буфер
	size_t packet_size = sizeof(w_files_header_t) + path_len + data_len;
	uint8_t *packet	   = (uint8_t *)malloc(packet_size);
	if (!packet)
	{
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -6;
	}
	memset(packet, 0, packet_size);

	w_files_header_t *hdr = (w_files_header_t *)packet;
	hdr->command		  = command;
	hdr->return_code	  = 0;
	hdr->request_id		  = g_current_request_id;
	hdr->offset			  = offset;
	hdr->data_length	  = data_len;
	hdr->path_length	  = (uint8_t)path_len;

	// Копируем path и data
	memcpy(packet + sizeof(w_files_header_t), path, path_len);
	if (data_len > 0 && data)
	{
		memcpy(packet + sizeof(w_files_header_t) + path_len, data, data_len);
	}

	// Очистим семафор перед отправкой
	xSemaphoreTake(g_response_sem, 0);

	// Отправляем
	int ret_send = Rdt_SendBlock(W_CHAN_FILES, packet, packet_size, NULL);
	if (ret_send != 0)
	{
		// Ошибка при отправке
		free(packet);
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		if (return_code) *return_code = W_FILES_ERR_INTERNAL;
		return -7;
	}

	// free(packet);

	// Готовимся принять ответ
	g_resp_return_code = 0xFF;
	g_resp_data_len	   = 0;
	// if (g_resp_buffer == NULL) g_resp_buffer = (uint8_t *)malloc(W_FILES_MAX_DATA);
	// if(!g_resp_buffer)
	// {
	// 	logE("Enomem");
	// 	g_request_in_progress = false;
	// 	xSemaphoreGive(g_mutex);
	// 	if (return_code) *return_code = W_FILES_ERR_INTERNAL;
	// 	return -7;
	// }
	// memset(g_resp_buffer, 0, W_FILES_MAX_DATA);

	// Если ранее был выделен буфер, освобождаем его
	if (g_resp_buffer)
	{
		free(g_resp_buffer);
		g_resp_buffer = NULL;
	}

	// Ждём ответа или таймаута
	if (xSemaphoreTake(g_response_sem, wait_ticks) == pdTRUE)
	{
		// Ответ получен
		if (return_code)
		{
			*return_code = g_resp_return_code;
		}
		// Копируем данные, если нужно
		if (out_data && inout_size)
		{
			size_t to_copy = (g_resp_data_len <= *inout_size) ? g_resp_data_len : *inout_size;
			memcpy(out_data, g_resp_buffer, to_copy);
			*inout_size = to_copy;
		}
		// Сбрасываем
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		return 0; // Успех (получили ответ)
	}
	else
	{
		// Таймаут
		if (return_code)
		{
			*return_code = W_FILES_ERR_INTERNAL; // Можно завести отдельный код W_FILES_ERR_TIMEOUT
		}
		g_request_in_progress = false;
		xSemaphoreGive(g_mutex);
		return -8;
	}
}

// ----------------------------------------------------------------
// Колбэк приёма на канале W_CHAN_FILES
// ----------------------------------------------------------------

static void w_files_receive_cb(void *handler_arg,
							   esp_event_base_t base,
							   int32_t id,
							   void *event_data)
{
	(void)handler_arg;
	(void)base;
	(void)id;
	(void)event_data;

	// Читаем пакет из Rdt
	rdt_block_item_t block_item;
	if (!Rdt_ReceiveBlock(W_CHAN_FILES, &block_item, 0))
	{
		// Нет блока
		return;
	}

	// Обработать пакет
	if (block_item.data_ptr && block_item.data_size >= sizeof(w_files_header_t))
	{
		w_files_handle_incoming_packet(block_item.data_ptr, block_item.data_size);
	}

	// Освободить
	Rdt_FreeReceivedBlock(&block_item);
}

static void w_files_handle_incoming_packet(const uint8_t *packet_data, size_t packet_size)
{
	const w_files_header_t *hdr = (const w_files_header_t *)packet_data;
	// Проверяем минимальный размер
	if (packet_size < sizeof(w_files_header_t))
	{
		return; // некорректный пакет
	}
	// Определяем, запрос это или ответ
	switch (hdr->command)
	{
		case W_FILES_CMD_LIST:
		case W_FILES_CMD_READ:
		case W_FILES_CMD_WRITE:
			// Это запрос от удалённого узла
			w_files_process_request(hdr, packet_size);
			break;

		case W_FILES_CMD_LIST_RESP:
		case W_FILES_CMD_READ_RESP:
		case W_FILES_CMD_WRITE_RESP:
			// Это ответ
			w_files_process_response(hdr, packet_size);
			break;

		default:
			// Неизвестная команда, можно проигнорировать
			break;
	}
}

// ----------------------------------------------------------------
// Обработка входящего ЗАПРОСА
// ----------------------------------------------------------------

static void w_files_process_request(const w_files_header_t *hdr_in, size_t packet_size)
{
	uint8_t command		= hdr_in->command;
	uint16_t request_id = hdr_in->request_id;
	uint8_t return_code = W_FILES_OK;

	// Извлечём путь и данные
	size_t path_len		  = hdr_in->path_length;
	size_t data_len		  = hdr_in->data_length;
	const uint8_t *p_path = (const uint8_t *)(hdr_in + 1);
	const uint8_t *p_data = p_path + path_len; // указатель на начало данных

	// Безопасная проверка
	if (sizeof(*hdr_in) + path_len + data_len > packet_size)
	{
		// Пакет "битый" или неполный
		return_code = W_FILES_ERR_INTERNAL;
	}
	else if (path_len > W_FILES_MAX_PATH || data_len > W_FILES_MAX_DATA)
	{
		return_code = W_FILES_ERR_TOOLARGE;
	}

	// Для формирования ответа будем динамически выделять буфер
	size_t resp_packet_size = sizeof(w_files_header_t) + W_FILES_MAX_DATA;
	uint8_t *resp_buf		= (uint8_t *)malloc(resp_packet_size);
	if (!resp_buf)
	{
		// Не удалось выделить память для ответа
		return;
	}
	memset(resp_buf, 0, resp_packet_size);

	w_files_header_t *hdr_out = (w_files_header_t *)resp_buf;
	hdr_out->command		  = command + 1; // например, LIST -> LIST_RESP (договорённость)
	hdr_out->return_code	  = return_code;
	hdr_out->request_id		  = request_id; // чтобы клиент сопоставил ответ
	hdr_out->offset			  = hdr_in->offset;
	hdr_out->data_length	  = 0;
	hdr_out->path_length	  = 0;

	// Если уже ошибка - просто отсылаем ответ с return_code
	if (return_code != W_FILES_OK)
	{
		size_t resp_size = sizeof(w_files_header_t);
		int ret			 = Rdt_SendBlock(W_CHAN_FILES, resp_buf, resp_size, NULL);
		if (ret != 0)
		{
			// Ошибка отправки, освобождаем память
			free(resp_buf);
		}
		return;
	}

	// Преобразуем p_path в null-terminated строку (скопируем во временный буфер)
	char path_buf[W_FILES_MAX_PATH + 1];
	memcpy(path_buf, p_path, path_len);
	path_buf[path_len] = '\0';

	// Выполняем действие
	if (command == W_FILES_CMD_LIST)
	{
		// Получаем список файлов с помощью портируемой функции
		// 1) Указатель на заголовок и на начало области данных
		w_files_header_t *hdr_out = (w_files_header_t *)resp_buf;
		u8 *resp_data			  = (u8 *)(resp_buf + sizeof(w_files_header_t));

		// Функция должна заполнить resp_buf после заголовка и установить data_length
		size_t len	= W_FILES_MAX_DATA;
		return_code = w_port_filelist_get(path_buf, resp_data, &len);
		if (return_code != W_FILES_OK)
		{
			// Если произошла ошибка, данные не важны
			hdr_out->data_length = 0;
		}
		else
		{
			hdr_out->data_length = len;
		}
	}
	else if (command == W_FILES_CMD_READ)
	{
		logI("Read req from %s size %d", path_buf, (int)data_len);
		// Открываем файл, w_port_fseek(offset), читаем <= W_FILES_MAX_DATA
		FILE *f = w_port_fopen(path_buf, "rb");
		if (!f)
		{
			return_code = W_FILES_ERR_NOFILE;
		}
		else
		{
			if (hdr_in->offset != 0xFFFFFFFF)
			{
				if (w_port_fseek(f, hdr_in->offset, SEEK_SET) != 0)
				{
					return_code = W_FILES_ERR_IO;
				}
			}
			if (return_code == W_FILES_OK)
			{
				// Считываем data_length, но не более W_FILES_MAX_DATA
				size_t len_to_read = (data_len > 0) ? data_len : W_FILES_MAX_DATA;
				if (len_to_read > W_FILES_MAX_DATA)
				{
					len_to_read = W_FILES_MAX_DATA;
				}
				size_t read_bytes	 = w_port_fread(resp_buf + sizeof(w_files_header_t), 1, len_to_read, f);
				hdr_out->data_length = (uint32_t)read_bytes;
				if (read_bytes < len_to_read && ferror(f))
				{
					return_code = W_FILES_ERR_IO;
				}
				w_port_fclose(f);
			}
			else
			{
				// Закрываем файл в случае ошибки
				w_port_fclose(f);
			}
		}
	}
	else if (command == W_FILES_CMD_WRITE)
	{
		// Открываем файл, если offset=0xFFFFFFFF => дописываем в конец
		// Иначе переходим на offset
		FILE *f = NULL;
		if (hdr_in->offset == 0xFFFFFFFF)
		{
			// append
			f = w_port_fopen(path_buf, "ab");
		}
		else
		{
			// запись по смещению — откроем "r+b" (создать, если нет?)
			f = w_port_fopen(path_buf, "r+b");
			if (!f)
			{
				// Попробуем создать
				f = w_port_fopen(path_buf, "wb");
			}
		}

		if (!f)
		{
			return_code = W_FILES_ERR_NOFILE;
		}
		else
		{
			if (hdr_in->offset != 0xFFFFFFFF)
			{
				if (w_port_fseek(f, hdr_in->offset, SEEK_SET) != 0)
				{
					return_code = W_FILES_ERR_IO;
				}
			}
			if (return_code == W_FILES_OK)
			{
				size_t written = w_port_fwrite(p_data, 1, data_len, f);
				if (written < data_len)
				{
					return_code = W_FILES_ERR_IO;
				}
				hdr_out->data_length = 0; // для WRITE_RESP обычно нет данных
			}
			w_port_fclose(f);
		}
	}
	else
	{
		// неизвестная команда
		return_code = W_FILES_ERR_UNKNOWN;
	}

	// Готовим ответ
	hdr_out->return_code   = return_code;
	size_t total_resp_size = sizeof(w_files_header_t) + hdr_out->data_length;

	// Отправляем ответ
	int ret = Rdt_SendBlock(W_CHAN_FILES, resp_buf, total_resp_size, NULL);
	if (ret != 0)
	{
		// Ошибка отправки, освобождаем память
		free(resp_buf);
	}
}

// ----------------------------------------------------------------
// Обработка входящего ОТВЕТА
// ----------------------------------------------------------------

static void w_files_process_response(const w_files_header_t *hdr_in, size_t packet_size)
{
	// Смотрим, активно ли у нас ожидание и совпадает ли request_id
	if (!g_request_in_progress || (hdr_in->request_id != g_current_request_id))
	{
		// Либо нет активного запроса, либо ID не совпадает
		return;
	}

	// Копируем return_code
	g_resp_return_code = hdr_in->return_code;

	// Извлекаем данные ответа, если есть
	size_t data_len		  = hdr_in->data_length;
	const uint8_t *p_data = (const uint8_t *)(hdr_in + 1);

	// Проверяем, что пакет содержит достаточно данных
	if (sizeof(*hdr_in) + data_len > packet_size)
	{
		// Некорректный пакет
		g_resp_return_code = W_FILES_ERR_INTERNAL;
		g_resp_data_len	   = 0;
	}
	else
	{
		// Выделяем память для ответа
		if (data_len > 0)
		{
			g_resp_buffer = (uint8_t *)malloc(data_len);
			if (g_resp_buffer)
			{
				memcpy(g_resp_buffer, p_data, data_len);
				g_resp_data_len = data_len;
			}
			else
			{
				// Не удалось выделить память
				g_resp_return_code = W_FILES_ERR_INTERNAL;
				g_resp_data_len	   = 0;
			}
		}
		else
		{
			// Нет данных
			g_resp_buffer	= NULL;
			g_resp_data_len = 0;
		}
	}

	// Снимаем задачу с блокировки (освобождаем семафор)
	xSemaphoreGive(g_response_sem);
}
