/*
 * ftp_data.c
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#include <stdarg.h>
#include "ftp_data.h"
#include "ftp_config.h"
#include "lwrb.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define FTP_BUF_SIZE_MIN 			1024
#define FTP_BUF_SIZE 				(FTP_BUF_SIZE_MIN * FTP_BUF_SIZE_MULT)
#define FTP_USER_NAME_OK(name)		(!strcmp(name, ftp_user_name))
#define FTP_USER_PASS_OK(pass)		(!strcmp(pass, ftp_user_pass))
#define FTP_IS_LOGGED_IN(p_ftp)		(p_ftp->user == FTP_USER_USER_LOGGED_IN)
#define PORT_INCREMENT_OFFSET		25 // used for a bugfix which works around ports which are already in use (from a previous connection)
#define FTP_DATE_STRING_SIZE		64

#define FTP_DATA_BUFFER_MESSAGE_SIZE		sizeof(ftp_data_msg_t)
#define FTP_DATA_BUFFER_SIZE				(FTP_DATA_BUFFER_MESSAGE_SIZE * 10)
#define FTP_DATA_TASK_SIZE					1024

typedef enum {
	FTP_USER_NONE,
	FTP_USER_USER_NO_PASS,
	FTP_USER_USER_LOGGED_IN
} ftp_user_t;

typedef enum {
	FTP_DATA_NONE,
	FTP_DATA_CMD,
	FTP_DATA_FILE,
} ftp_send_data_type_t;

typedef struct {
	uint16_t data_port;
	uint8_t data_port_incremented;
	FIL file;
	FILINFO finfo;
	char path_rename[FTP_CWD_SIZE];
	char path[FTP_CWD_SIZE];
	ALIGN_32BYTES(char ftp_buff[FTP_BUF_SIZE + 1]);
	lwrb_t lwrb;
	char date_str[FTP_DATE_STRING_SIZE];
	uint8_t ftp_con_num;
	ftp_user_t user;
	dcm_type data_conn_mode;
	ftp_send_data_type_t data_send_type;
} ftp_data_t;

static char ftp_user_name[FTP_USER_NAME_LEN + 1] = FTP_USER_NAME_DEFAULT;
static char ftp_user_pass[FTP_USER_PASS_LEN + 1] = FTP_USER_PASS_DEFAULT;
FTP_STRUCT_MEM_SECTION(static ftp_data_t ftp_data[FTP_NBR_CLIENTS]) = {0};
static SemaphoreHandle_t data_sent_sem = { 0 };
static StreamBufferHandle_t ftp_data_buffer_handle = NULL;
static TaskHandle_t ftp_data_task_handle = NULL;
static bool inited = false;

static void ftp_data_clear(uint8_t index) {
	memset(&(ftp_data[index]), 0, sizeof(ftp_data_t));
}

ftp_result_t ftp_send(const ftp_cmd_msg_client_t *const client, const char *fmt, ...) {
	if (xSemaphoreTake(data_sent_sem, 2000) == pdTRUE) {
		ftp_data_t *pftp_data = &ftp_data[client->index];
		pftp_data->data_send_type = FTP_DATA_CMD;
		char *pbuff = (char*) lwrb_get_linear_block_write_address(&(pftp_data->lwrb));
		lwrb_sz_t available_write_len = lwrb_get_linear_block_write_length(&(pftp_data->lwrb));

		va_list args;
		va_start(args, fmt);
		int full_len = vsnprintf(pbuff, available_write_len, fmt, args);
		va_end(args);

		if (full_len <= 0) {
			DEBUG_PRINT(client->index, "Error on parsing data\r\n");
			return (FTP_RES_ERROR);
		} else if (full_len >= available_write_len) {
			DEBUG_PRINT(client->index, "Data NOT written fully\r\n");
			return (FTP_RES_ERROR);
		} else {
			lwrb_advance(&(pftp_data->lwrb), full_len);
			err_t err = tcp_write(client->tpcb, pbuff, full_len, 0);
			if (err == ERR_OK) {
				DEBUG_PRINT(client->index, "Data fully written\r\n");
				return (FTP_RES_OK);
			} else {
				DEBUG_PRINT(client->index, "Error on sending data: %d\r\n", err);
				return (FTP_RES_ERROR);
			}
		}
	} else {
		DEBUG_PRINT(client->index, "Timeout on data sent semaphore\r\n");
		return (FTP_RES_TIMEOUT);
	}
}

err_t ftp_data_sent(const ftp_cmd_msg_client_t *const client, uint16_t len) {
	ftp_data_t *pftp_data = &ftp_data[client->index];
	if (pftp_data->data_send_type == FTP_DATA_CMD) {
		lwrb_skip(&(pftp_data->lwrb), len);
		lwrb_reset(&(pftp_data->lwrb));
	} else if (pftp_data->data_send_type == FTP_DATA_FILE) {
		lwrb_skip(&(pftp_data->lwrb), len);
	} else {
		DEBUG_PRINT(index, "Unknown sent data type: %d\r\n", pftp_data->data_send_type);
		lwrb_reset(&(pftp_data->lwrb));
	}
	xSemaphoreGive(data_sent_sem);
	return (ERR_OK);
}

bool ftp_is_logged_in(const ftp_cmd_msg_client_t *const client) {
	return (ftp_data[client->index].user == FTP_USER_USER_LOGGED_IN);
}

char* ftp_get_path(const ftp_cmd_msg_client_t *const client) {
	return (ftp_data[client->index].path);
}

void ftp_set_connection_mode(const ftp_cmd_msg_client_t *const client, dcm_type data_conn_mode) {
	ftp_data[client->index].data_conn_mode = data_conn_mode;
}

dcm_type ftp_get_connection_mode(const ftp_cmd_msg_client_t *const client) {
	return (ftp_data[client->index].data_conn_mode);
}

__NO_RETURN static void ftp_data_task(void *pvParameters) {
	UNUSED(pvParameters);
	ftp_data_msg_t msg = { 0 };

	for (;;) {
		if (xStreamBufferReceive(ftp_data_buffer_handle, &msg, FTP_DATA_BUFFER_MESSAGE_SIZE, portMAX_DELAY) != FTP_DATA_BUFFER_MESSAGE_SIZE) {
			continue;
		}
		// TODO HANDLE DATA HERE
		if (msg.p) {
			pbuf_free(msg.p);
		}
	}
}

err_t ftp_data_handle(const ftp_data_msg_t *const msg) {
	if (msg->p) {
		pbuf_ref(msg->p);
	}
	if (xStreamBufferSend(ftp_data_buffer_handle, msg, FTP_DATA_BUFFER_MESSAGE_SIZE, 0) == FTP_DATA_BUFFER_MESSAGE_SIZE) {
		DEBUG_PRINT(index, "ftp data buff send OK\r\n");
	} else {
		DEBUG_PRINT(index, "ftp data buff send ERROR\r\n");
	}
	return (ERR_OK);
}

void ftp_data_init(void) {
	if (!inited) {
		inited = true;

		data_sent_sem = xSemaphoreCreateBinary();
		assert_param(data_sent_sem != NULL);
		xSemaphoreGive(data_sent_sem);

		ftp_data_buffer_handle = xStreamBufferCreate(FTP_DATA_BUFFER_SIZE, FTP_DATA_BUFFER_MESSAGE_SIZE);
		assert_param(ftp_data_buffer_handle != NULL);
		assert_param(xTaskCreate(ftp_data_task, "FTP DATA", FTP_DATA_TASK_SIZE, NULL, osPriorityNormal, &ftp_data_task_handle) == pdPASS);
	}
}
