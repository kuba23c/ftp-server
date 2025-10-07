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
#include "stream_buffer.h"

#define FTP_DATA_BUFFER_MESSAGE_SIZE		sizeof(ftp_data_msg_t)
#define FTP_DATA_BUFFER_SIZE				(FTP_DATA_BUFFER_MESSAGE_SIZE * 10)
#define FTP_DATA_TASK_SIZE					1024
#define FTP_DATA_MUTEX_WAIT_TIME			2000

typedef struct {
	FIL file;
	FILINFO finfo;
	DIR dir;
} ftp_data_t;

typedef struct {
	ftp_data_t data[FTP_NBR_CLIENTS];
	ftp_data_stats_t stats;
	bool inited;
} ftp_datas_t;

FTP_STRUCT_MEM_SECTION(static ftp_datas_t ftp_datas) = { 0 };
static StreamBufferHandle_t ftp_data_buffer_handle = NULL;
static TaskHandle_t ftp_data_task_handle = NULL;

__NO_RETURN static void ftp_data_task(void *pvParameters) {
	UNUSED(pvParameters);
	ftp_data_msg_t msg = { 0 };

	for (;;) {
		if (xStreamBufferReceive(ftp_data_buffer_handle, &msg, FTP_DATA_BUFFER_MESSAGE_SIZE, portMAX_DELAY) != FTP_DATA_BUFFER_MESSAGE_SIZE) {
			continue;
		}
		switch (msg.msg_type) {
		case FTP_DATA_MSG_RECV:
			// TODO HANDLE DATA HERE
			if (msg.data.recv_p) {
				pbuf_free(msg.data.recv_p);
			}
			break;
		case FTP_DATA_MSG_SENT:
			// TODO HANDLE DATA HERE
			break;
		case FTP_DATA_MSG_START_RX:
			// TODO HANDLE DATA HERE
			break;
		case FTP_DATA_MSG_START_TX:
			// TODO HANDLE DATA HERE
			break;
		case FTP_DATA_MSG_STOP:
			// TODO HANDLE DATA HERE
			break;
		default:
			DEBUG_PRINT(msg.index, "DATA: UNKNOWN MESSAGE TYPE: %d !!!\r\n", msg.msg_type);
			break;
		}
	}
}

err_t ftp_data_handle(const ftp_data_msg_t *const msg) {
	if (xStreamBufferSend(ftp_data_buffer_handle, msg, FTP_DATA_BUFFER_MESSAGE_SIZE, 0) == FTP_DATA_BUFFER_MESSAGE_SIZE) {
		DEBUG_PRINT(msg->index, "ftp data buff send OK\r\n");
	} else {
		DEBUG_PRINT(msg->index, "ftp data buff send ERROR\r\n");
	}
	return (ERR_OK);
}

void ftp_data_init(void) {
	if (!ftp_datas.inited) {
		ftp_datas.inited = true;
		ftp_data_buffer_handle = xStreamBufferCreate(FTP_DATA_BUFFER_SIZE, FTP_DATA_BUFFER_MESSAGE_SIZE);
		assert_param(ftp_data_buffer_handle != NULL);
		assert_param(xTaskCreate(ftp_data_task, "FTP DATA", FTP_DATA_TASK_SIZE, NULL, osPriorityNormal, &ftp_data_task_handle) == pdPASS);
		assert_param(ftp_data_task_handle != NULL);
	}
}
