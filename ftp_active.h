/*
 * ftp_active.h
 *
 *  Created on: Oct 5, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_ACTIVE_H_
#define FTP_SERVER_FTP_ACTIVE_H_

#include <stdint.h>
#include "lwip.h"
#include "tcp.h"

typedef struct {
	uint32_t active_rejected;
	uint32_t active_tcp_stack_error;
	uint32_t active_accepted;
	uint32_t active_errors;
	uint32_t active_closed;
	uint32_t active_connected;
	uint32_t active_timeouts;
} ftp_active_stats_t;

void ftp_active_set_ip(uint8_t index, uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void ftp_active_set_port(uint8_t index, uint16_t port);
err_t ftp_active_connect(uint8_t index);

#endif /* FTP_SERVER_FTP_ACTIVE_H_ */
