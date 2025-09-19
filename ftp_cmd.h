/*
 * ftp_cmd.h
 *
 *  Created on: Sep 19, 2025
 *      Author: jakub czekaj
 */

#ifndef FTP_SERVER_FTP_CMD_H_
#define FTP_SERVER_FTP_CMD_H_

#include "lwip.h"

err_t ftp_cmd_handle(uint8_t index, struct tcp_pcb *tpcb, struct pbuf *p);

#endif /* FTP_SERVER_FTP_CMD_H_ */
