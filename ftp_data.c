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
#include "ftp_data_connection.h"
#include "ftp_client.h"

#define FTP_DATA_BUFFER_MESSAGE_SIZE		sizeof(ftp_data_msg_t)
#define FTP_DATA_BUFFER_SIZE				(FTP_DATA_BUFFER_MESSAGE_SIZE * 10)
#define FTP_DATA_TASK_SIZE					1024
#define FTP_DATA_MUTEX_WAIT_TIME			2000

typedef struct {
	uint8_t index;
	FIL file;
	FILINFO finfo;
	DIR dir;
	ftp_data_msg_type_t type;
	bool is_list;
	lwrb_t *lwrb;
	char *addr;
	uint32_t available_len;
	uint32_t written_len;
	uint32_t all_bytes_transfered;
	int temp_len;
	bool not_finished;
	char *path;
} ftp_data_t;

typedef struct {
	ftp_data_t data[FTP_NBR_CLIENTS];
	ftp_data_stats_t stats;
	bool inited;
} ftp_datas_t;

FTP_STRUCT_MEM_SECTION(static ftp_datas_t ftp_datas) = { 0 };
static StreamBufferHandle_t ftp_data_buffer_handle = NULL;
static TaskHandle_t ftp_data_task_handle = NULL;

static void ftp_data_list_handle(ftp_data_t *data) {
	if (!data->not_finished) {
		data->type = FTP_DATA_MSG_LIST;
		data->lwrb = ftp_data_get_lwrb(data->index);
		data->addr = lwrb_get_linear_block_write_address(data->lwrb);
		data->available_len = lwrb_get_linear_block_write_length(data->lwrb);
		data->written_len = 0;
		data->all_bytes_transfered = 0;
	} else {
		data->addr = lwrb_get_linear_block_write_address(data->lwrb);
		data->available_len = lwrb_get_linear_block_write_length(data->lwrb);
		data->written_len = 0;

		if (data->is_list) {
			data->temp_len = snprintf(data->addr, data->available_len, "%s\r\n", data->finfo.fname);
		} else if (data->finfo.fattrib & AM_DIR) {
			data->temp_len = snprintf(data->addr, data->available_len, "+/,\t%s\r\n", data->finfo.fname);
		} else {
			data->temp_len = snprintf(data->addr, data->available_len, "+r,s%ld,\t%s\r\n", data->finfo.fsize, data->finfo.fname);
		}
		if (data->temp_len < data->available_len) {
			data->available_len -= data->temp_len;
			data->written_len += data->temp_len;
		} else {
			return;
		}
	}

	while (FTP_F_READDIR(&(data->dir), &(data->finfo)) == FR_OK) {
		if (data->finfo.fname[0] == 0) {
			data->not_finished = false;
			FTP_F_CLOSEDIR(&(data->dir));
			break;
		}
		if (data->finfo.fname[0] == '.') {
			continue;
		}
		if (data->is_list) {
			data->temp_len = snprintf(data->addr, data->available_len, "%s\r\n", data->finfo.fname);
		} else if (data->finfo.fattrib & AM_DIR) {
			data->temp_len = snprintf(data->addr, data->available_len, "+/,\t%s\r\n", data->finfo.fname);
		} else {
			data->temp_len = snprintf(data->addr, data->available_len, "+r,s%ld,\t%s\r\n", data->finfo.fsize, data->finfo.fname);
		}
		if (data->temp_len < data->available_len) {
			data->available_len -= data->temp_len;
			data->written_len += data->temp_len;
			data->all_bytes_transfered += data->temp_len;
		} else {
			data->not_finished = true;
			break;
		}
	}

	lwrb_advance(data->lwrb, data->written_len);
	ftp_data_send(data->index);
}

static ftp_result_t ftp_data_tx_handle(ftp_data_t *data) {
	if (!data->not_finished) {
		data->type = FTP_DATA_MSG_START_TX;
		data->lwrb = ftp_data_get_lwrb(data->index);
		data->addr = lwrb_get_linear_block_write_address(data->lwrb);
		data->available_len = lwrb_get_linear_block_write_length(data->lwrb);
		data->written_len = 0;
		data->all_bytes_transfered = 0;
	} else {
		data->addr = lwrb_get_linear_block_write_address(data->lwrb);
		data->available_len = lwrb_get_linear_block_write_length(data->lwrb);
		data->written_len = 0;
	}

	if (FTP_F_READ(&(data->file), data->addr, data->available_len, (UINT* ) &(data->written_len)) != FR_OK) {
		ftp_cmd_resp_send(data->index, "451 Error on file read\r\n");
		FTP_F_CLOSE(&(data->file));
		path_up_a_level(data->path);
		return (FTP_RES_ERROR);
	}
	if (data->written_len < data->available_len) {
		data->not_finished = false;
		FTP_F_CLOSE(&(data->file));
		path_up_a_level(data->path);
	} else {
		data->not_finished = true;
	}

	lwrb_advance(data->lwrb, data->written_len);
	ftp_data_send(data->index);
	return (FTP_RES_OK);
}

__NO_RETURN static void ftp_data_task(void *pvParameters) {
	UNUSED(pvParameters);
	ftp_data_msg_t msg = { 0 };
	ftp_data_t *data = NULL;
	for (;;) {
		if (xStreamBufferReceive(ftp_data_buffer_handle, &msg, FTP_DATA_BUFFER_MESSAGE_SIZE, portMAX_DELAY) != FTP_DATA_BUFFER_MESSAGE_SIZE) {
			continue;
		}
		data = &(ftp_datas.data[msg.index]);

		switch (msg.msg_type) {
		case FTP_DATA_MSG_RECV:
			// TODO HANDLE DATA HERE
			if (msg.data.recv_p) {
				pbuf_free(msg.data.recv_p);
			}
			break;
		case FTP_DATA_MSG_SENT:
			if (data->type == FTP_DATA_MSG_LIST) {
				if (ftp_data_lock(msg.index) != FTP_RES_OK) {
					ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", data->finfo.fname);
					FTP_F_CLOSEDIR(&(data->dir));
					ftp_data_conn_stop_ex(msg.index);
					break;
				}
				if (data->not_finished) {
					lwrb_skip(data->lwrb, msg.data.sent_len);
					ftp_data_list_handle(data);
				} else if (lwrb_get_full(data->lwrb) == 0) {
					data->type = FTP_DATA_MSG_NONE;
					DEBUG_PRINT(msg.index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "226 Directory send OK, %lu bytes\r\n", data->all_bytes_transfered);
					ftp_data_conn_stop_ex(msg.index);
				}
				ftp_data_unlock(msg.index);
			} else if (data->type == FTP_DATA_MSG_START_TX) {
				if (ftp_data_lock(msg.index) != FTP_RES_OK) {
					ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", msg.data.tx.parameters);
					FTP_F_CLOSE(&(data->file));
					path_up_a_level(msg.data.tx.path);
					ftp_data_conn_stop_ex(msg.index);
					break;
				}
				if (data->not_finished) {
					lwrb_skip(data->lwrb, msg.data.sent_len);
					if (ftp_data_tx_handle(data) != FTP_RES_OK) {
						ftp_data_conn_stop_ex(msg.index);
					}
				} else if (lwrb_get_full(data->lwrb) == 0) {
					data->type = FTP_DATA_MSG_NONE;
					DEBUG_PRINT(msg.index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "226 File successfully transferred, %lu bytes\r\n", data->all_bytes_transfered);
					ftp_data_conn_stop_ex(msg.index);
				}
				ftp_data_unlock(msg.index);
			} else if (data->type == FTP_DATA_MSG_START_RX) {
				// TODO HANDLE DATA HERE
			}
			break;
		case FTP_DATA_MSG_START_RX:
			// TODO HANDLE DATA HERE
			break;
		case FTP_DATA_MSG_START_TX:
			if (data->type != FTP_DATA_MSG_NONE) {
				ftp_cmd_resp_send(msg.index, "550 Other task is running: %d\r\n", data->type);
				path_up_a_level(msg.data.tx.path);
				pbuf_free(msg.data.tx.p);
				break;
			}
			if (FTP_F_STAT(msg.data.tx.path, &data->finfo) != FR_OK) {
				ftp_cmd_resp_send(msg.index, "550 File %s not found\r\n", msg.data.tx.parameters);
				path_up_a_level(msg.data.tx.path);
				pbuf_free(msg.data.tx.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (FTP_F_OPEN(&(data->file), msg.data.tx.path, FA_READ) != FR_OK) {
				ftp_cmd_resp_send(msg.index, "550 Can't open %s\r\n", msg.data.tx.parameters);
				path_up_a_level(msg.data.tx.path);
				pbuf_free(msg.data.tx.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (ftp_data_lock(msg.index) != FTP_RES_OK) {
				ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", msg.data.tx.parameters);
				FTP_F_CLOSE(&(data->file));
				path_up_a_level(msg.data.tx.path);
				pbuf_free(msg.data.tx.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			data->path = msg.data.tx.path;
			data->index = msg.index;
			pbuf_free(msg.data.tx.p);
			ftp_data_tx_handle(data);
			ftp_data_unlock(msg.index);
			break;
		case FTP_DATA_MSG_LIST:
			if (data->type != FTP_DATA_MSG_NONE) {
				ftp_cmd_resp_send(msg.index, "550 Other task is running: %d\r\n", data->type);
				pbuf_free(msg.data.list.p);
				break;
			}
			if (FTP_F_OPENDIR(&(data->dir), msg.data.list.parameters) != FR_OK) {
				ftp_cmd_resp_send(msg.index, "550 Can't open directory %s\r\n", msg.data.list.parameters);
				pbuf_free(msg.data.list.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (ftp_data_lock(msg.index) != FTP_RES_OK) {
				ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", msg.data.list.parameters);
				FTP_F_CLOSEDIR(&(data->dir));
				pbuf_free(msg.data.list.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (strcmp(msg.data.list.command, "LIST")) {
				data->is_list = true;
			} else {
				data->is_list = false;
			}
			if (FTP_F_STAT(msg.data.list.parameters, &(data->finfo)) != FR_OK) {
				data->finfo.fname[0] = '/';
				data->finfo.fname[1] = 0;
			}
			data->index = msg.index;
			pbuf_free(msg.data.list.p);
			ftp_data_list_handle(data);
			ftp_data_unlock(msg.index);
			break;
		case FTP_DATA_MSG_STOP:
			if (data->type == FTP_DATA_MSG_LIST) {
				ftp_cmd_resp_send(msg.index, "451 LIST cmd stop\r\n");
				FTP_F_CLOSEDIR(&(data->dir));
				data->type = FTP_DATA_MSG_NONE;
			} else if (data->type == FTP_DATA_MSG_START_TX) {
				ftp_cmd_resp_send(msg.index, "451 RETR cmd stop\r\n");
				FTP_F_CLOSE(&(data->file));
				path_up_a_level(data->path);
				data->type = FTP_DATA_MSG_NONE;
			} else if (data->type == FTP_DATA_MSG_START_RX) {
				ftp_cmd_resp_send(msg.index, "451 STOR cmd stop\r\n");
				FTP_F_CLOSE(&(data->file));
				// TODO
//				path_up_a_level(data->path);
				data->type = FTP_DATA_MSG_NONE;
			}
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
