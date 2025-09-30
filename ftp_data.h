/*
 * ftp_data.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_DATA_H_
#define FTP_SERVER_FTP_DATA_H_

#include <stdint.h>
#include "lwip.h"
#include "tcp.h"

void ftp_data_clear(uint8_t index);
err_t ftp_data_sent(uint8_t index, struct tcp_pcb *tpcb, uint16_t len);

#endif /* FTP_SERVER_FTP_DATA_H_ */
