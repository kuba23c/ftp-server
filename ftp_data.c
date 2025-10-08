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
	bool connected;
	bool data_waits;
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

		if (!data->is_list) {
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
		if (!data->is_list) {
			data->temp_len = snprintf(data->addr + data->written_len, data->available_len, "%s\r\n", data->finfo.fname);
		} else if (data->finfo.fattrib & AM_DIR) {
			data->temp_len = snprintf(data->addr + data->written_len, data->available_len, "+/,\t%s\r\n", data->finfo.fname);
		} else {
			data->temp_len = snprintf(data->addr + data->written_len, data->available_len, "+r,s%ld,\t%s\r\n", data->finfo.fsize, data->finfo.fname);
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
	if (lwrb_get_full(data->lwrb) == 0) {
		data->type = FTP_DATA_MSG_NONE;
		data->connected = false;
		data->data_waits = false;
		DEBUG_PRINT(data->index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
		ftp_cmd_resp_send(data->index, "226 Directory send OK, %lu bytes\r\n", data->all_bytes_transfered);
		ftp_data_conn_stop_ex(data->index);
		lwrb_reset(data->lwrb);
	} else if (data->connected) {
		ftp_data_send(data->index);
		DEBUG_PRINT(data->index, "DATA list send, connected\r\n");
	} else {
		data->data_waits = true;
		DEBUG_PRINT(data->index, "DATA list not send, not connected\r\n");
	}
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
	if (lwrb_get_full(data->lwrb) == 0) {
		data->type = FTP_DATA_MSG_NONE;
		data->connected = false;
		data->data_waits = false;
		DEBUG_PRINT(data->index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
		ftp_cmd_resp_send(data->index, "226 File successfully transferred, %lu bytes\r\n", data->all_bytes_transfered);
		ftp_data_conn_stop_ex(data->index);
		lwrb_reset(data->lwrb);
	} else if (data->connected) {
		ftp_data_send(data->index);
		DEBUG_PRINT(data->index, "DATA tx send, connected\r\n");
	} else {
		data->data_waits = true;
		DEBUG_PRINT(data->index, "DATA tx not send, not connected\r\n");
	}
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
			if (data->type == FTP_DATA_MSG_START_RX) {
				if (msg.data.recv_p == NULL) {
					data->type = FTP_DATA_MSG_NONE;
					FTP_F_CLOSE(&(data->file));
					DEBUG_PRINT(msg.index, "Received %lu bytes\r\n", data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "226 File successfully transferred, %lu bytes\r\n", data->all_bytes_transfered);
					path_up_a_level(msg.data.tx.path);
					ftp_data_conn_stop_ex(msg.index);
					break;
				}
				FRESULT result = FTP_F_WRITE(&data->file, msg.data.recv_p->payload, msg.data.recv_p->len, (UINT* )&(data->written_len));
				if (result != FR_OK || msg.data.recv_p->len != data->written_len) {
					data->type = FTP_DATA_MSG_NONE;
					FTP_F_CLOSE(&(data->file));
					DEBUG_PRINT(msg.index, "file write ERROR: %d, Received %lu bytes\r\n", result, data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "451 Communication error during transfer\r\n");
					path_up_a_level(msg.data.tx.path);
					ftp_data_conn_stop_ex(msg.index);
					break;
				}
			}
			pbuf_free(msg.data.recv_p);
			break;
		case FTP_DATA_MSG_SENT:
			if (data->type == FTP_DATA_MSG_LIST) {
				if (ftp_data_lock(msg.index) != FTP_RES_OK) {
					data->type = FTP_DATA_MSG_NONE;
					data->connected = false;
					data->data_waits = false;
					ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing folder\r\n");
					FTP_F_CLOSEDIR(&(data->dir));
					ftp_data_conn_stop_ex(msg.index);
					lwrb_reset(data->lwrb);
					break;
				}
				DEBUG_PRINT(data->index, "DATA sent len: %d\r\n", msg.data.sent_len);
				if (data->not_finished) {
					DEBUG_PRINT(data->index, "DATA not finished...\r\n");
					ftp_data_list_handle(data);
				} else if (lwrb_get_full(data->lwrb) == 0) {
					data->type = FTP_DATA_MSG_NONE;
					data->connected = false;
					data->data_waits = false;
					DEBUG_PRINT(msg.index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "226 Directory send OK, %lu bytes\r\n", data->all_bytes_transfered);
					ftp_data_conn_stop_ex(msg.index);
					lwrb_reset(data->lwrb);
				} else {
					DEBUG_PRINT(data->index, "DATA waiting for transfer end...\r\n");
				}
				ftp_data_unlock(msg.index);
			} else if (data->type == FTP_DATA_MSG_START_TX) {
				if (ftp_data_lock(msg.index) != FTP_RES_OK) {
					data->type = FTP_DATA_MSG_NONE;
					data->connected = false;
					data->data_waits = false;
					ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", data->finfo.fname);
					FTP_F_CLOSE(&(data->file));
					path_up_a_level(msg.data.tx.path);
					ftp_data_conn_stop_ex(msg.index);
					lwrb_reset(data->lwrb);
					break;
				}
				DEBUG_PRINT(data->index, "DATA sent len: %d\r\n", msg.data.sent_len);
				if (data->not_finished) {
					DEBUG_PRINT(data->index, "DATA not finished...\r\n");
					if (ftp_data_tx_handle(data) != FTP_RES_OK) {
						DEBUG_PRINT(data->index, "DATA read file error, Something went wrong\r\n");
						ftp_data_conn_stop_ex(msg.index);
					}
				} else if (lwrb_get_full(data->lwrb) == 0) {
					data->type = FTP_DATA_MSG_NONE;
					data->connected = false;
					data->data_waits = false;
					DEBUG_PRINT(msg.index, "Sent %lu bytes\r\n", data->all_bytes_transfered);
					ftp_cmd_resp_send(msg.index, "226 File successfully transferred, %lu bytes\r\n", data->all_bytes_transfered);
					ftp_data_conn_stop_ex(msg.index);
					lwrb_reset(data->lwrb);
				} else {
					DEBUG_PRINT(data->index, "DATA waiting for transfer end...\r\n");
				}
				ftp_data_unlock(msg.index);
			}
			break;
		case FTP_DATA_MSG_START_RX:
			if (data->type != FTP_DATA_MSG_NONE) {
				ftp_cmd_resp_send(msg.index, "550 Other task is running: %d\r\n", data->type);
				path_up_a_level(msg.data.rx.path);
				pbuf_free(msg.data.rx.p);
				break;
			}

			if (FTP_F_OPEN(&(data->file), msg.data.rx.path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
				ftp_cmd_resp_send(msg.index, "450 Can't open/create %s\r\n", msg.data.rx.parameters);
				path_up_a_level(msg.data.rx.path);
				pbuf_free(msg.data.rx.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (FTP_F_STAT(msg.data.rx.path, &data->finfo) != FR_OK) {
				FTP_F_CLOSE(&(data->file));
				ftp_cmd_resp_send(msg.index, "550 File %s not found\r\n", msg.data.rx.parameters);
				path_up_a_level(msg.data.rx.path);
				pbuf_free(msg.data.rx.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			data->path = msg.data.tx.path;
			data->index = msg.index;
			data->not_finished = false;
			pbuf_free(msg.data.tx.p);
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
			data->not_finished = false;
			pbuf_free(msg.data.tx.p);
			ftp_data_tx_handle(data);
			ftp_data_unlock(msg.index);
			break;
		case FTP_DATA_MSG_LIST:
			DEBUG_PRINT(msg.index, "ftp data list msg: begin\r\n");
			if (data->type != FTP_DATA_MSG_NONE) {
				DEBUG_PRINT(msg.index, "ftp data list msg: Other task is running: %d\r\n", data->type);
				ftp_cmd_resp_send(msg.index, "550 Other task is running: %d\r\n", data->type);
				pbuf_free(msg.data.list.p);
				break;
			}
			if (FTP_F_OPENDIR(&(data->dir), msg.data.list.parameters) != FR_OK) {
				DEBUG_PRINT(msg.index, "ftp data list msg: Can't open directory %s\r\n", msg.data.list.parameters);
				ftp_cmd_resp_send(msg.index, "550 Can't open directory %s\r\n", msg.data.list.parameters);
				pbuf_free(msg.data.list.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (ftp_data_lock(msg.index) != FTP_RES_OK) {
				DEBUG_PRINT(msg.index, "ftp data list msg: Can't lock task, closing %s\r\n", msg.data.list.parameters);
				ftp_cmd_resp_send(msg.index, "550 Can't lock task, closing %s\r\n", msg.data.list.parameters);
				FTP_F_CLOSEDIR(&(data->dir));
				pbuf_free(msg.data.list.p);
				ftp_data_conn_stop_ex(msg.index);
				break;
			}
			if (!strncmp(msg.data.list.command, "LIST", 4)) {
				data->is_list = true;
			} else {
				data->is_list = false;
			}
			data->index = msg.index;
			data->not_finished = false;
			pbuf_free(msg.data.list.p);
			ftp_data_list_handle(data);
			ftp_data_unlock(msg.index);
			break;
		case FTP_DATA_MSG_STOP:
			data->connected = false;
			data->data_waits = false;
			if (data->type == FTP_DATA_MSG_LIST) {
				lwrb_reset(data->lwrb);
				ftp_cmd_resp_send(msg.index, "451 LIST cmd stop\r\n");
				FTP_F_CLOSEDIR(&(data->dir));
				data->type = FTP_DATA_MSG_NONE;
			} else if (data->type == FTP_DATA_MSG_START_TX) {
				lwrb_reset(data->lwrb);
				ftp_cmd_resp_send(msg.index, "451 RETR cmd stop\r\n");
				FTP_F_CLOSE(&(data->file));
				path_up_a_level(data->path);
				data->type = FTP_DATA_MSG_NONE;
			} else if (data->type == FTP_DATA_MSG_START_RX) {
				ftp_cmd_resp_send(msg.index, "451 STOR cmd stop\r\n");
				FTP_F_CLOSE(&(data->file));
				path_up_a_level(data->path);
				data->type = FTP_DATA_MSG_NONE;
			}
			break;
		case FTP_DATA_MSG_CONNECTED:
			if (data->type == FTP_DATA_MSG_LIST || data->type == FTP_DATA_MSG_START_TX) {
				data->connected = true;
				DEBUG_PRINT(data->index, "DATA connected\r\n");
				if (data->data_waits) {
					data->data_waits = false;
					ftp_data_send(data->index);
					DEBUG_PRINT(data->index, "DATA connected, send data\r\n");
				}
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
