/*
 * ftp_cmd.h
 *
 *  Created on: Sep 19, 2025
 *      Author: jakub czekaj
 */

#ifndef FTP_SERVER_FTP_CMD_H_
#define FTP_SERVER_FTP_CMD_H_

#include "lwip.h"
#include "tcp.h"

#define DEBUG_PRINT(i, f, ...)	FTP_LOG_PRINT("[%d] "f, i, ##__VA_ARGS__)

typedef enum {
	FTP_RES_OK,
	FTP_RES_TIMEOUT,
	FTP_RES_ERROR
} ftp_result_t;

typedef struct __PACKED {
	uint8_t index;
	struct tcp_pcb *tpcb;
} ftp_cmd_msg_client_t;

typedef struct __PACKED {
	ftp_cmd_msg_client_t client;
	struct pbuf *p;
} ftp_cmd_msg_t;

err_t ftp_cmd_handle(const ftp_cmd_msg_t *const msg);
void ftp_cmd_init(void);

#endif /* FTP_SERVER_FTP_CMD_H_ */
