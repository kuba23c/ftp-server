/*
 * ftp_cmd.h
 *
 *  Created on: Sep 19, 2025
 *      Author: jakub czekaj
 */

#ifndef FTP_SERVER_FTP_CMD_H_
#define FTP_SERVER_FTP_CMD_H_

#include "lwip.h"

typedef struct __PACKED {
	uint8_t index;
	struct tcp_pcb *tpcb;
	struct pbuf *p;
} ftp_cmd_msg_t;

err_t ftp_cmd_handle(const ftp_cmd_msg_t *const msg);
void ftp_cmd_init(void);

#endif /* FTP_SERVER_FTP_CMD_H_ */
