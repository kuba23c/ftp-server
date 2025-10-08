/*
 * ftp_data.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_DATA_H_
#define FTP_SERVER_FTP_DATA_H_

#include <stdint.h>
#include <stdbool.h>
#include "ftp_cmd.h"

#define FTP_CWD_SIZE				_MAX_LFN + 8

typedef struct {
	uint32_t temp;
// TODO
} ftp_data_stats_t;

typedef enum {
	FTP_DATA_MSG_NONE,
	FTP_DATA_MSG_RECV,
	FTP_DATA_MSG_SENT,
	FTP_DATA_MSG_START_RX,
	FTP_DATA_MSG_START_TX,
	FTP_DATA_MSG_LIST,
	FTP_DATA_MSG_STOP,
	FTP_DATA_MSG_CONNECTED,
} ftp_data_msg_type_t;

typedef struct {
	struct pbuf *p;
	char *parameters;
	char *command;
} ftp_data_msg_data_params_t;

typedef struct {
	struct pbuf *p;
	char *parameters;
	char *path;
} ftp_data_msg_data_rx_tx_t;

typedef union __PACKED {
	struct pbuf *recv_p;
	uint16_t sent_len;
	void *stop;
	ftp_data_msg_data_params_t list;
	ftp_data_msg_data_rx_tx_t tx;
	ftp_data_msg_data_rx_tx_t rx;
} ftp_data_msg_data_t;

typedef struct __PACKED {
	ftp_data_msg_type_t msg_type;
	uint8_t index;
	ftp_data_msg_data_t data;
} ftp_data_msg_t;

err_t ftp_data_handle(const ftp_data_msg_t *const msg);
void ftp_data_init(void);

#endif /* FTP_SERVER_FTP_DATA_H_ */
