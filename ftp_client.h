/*
 * ftp_client.h
 *
 *  Created on: Sep 19, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_CLIENT_H_
#define FTP_SERVER_FTP_CLIENT_H_

#include <stdint.h>
#include "lwip.h"
#include "tcp.h"

typedef struct {
	uint8_t clients_connected;
	uint8_t clients_max;
	uint32_t clients_rejected;
	uint32_t clients_accepted;
	uint32_t clients_timeouts;
	uint32_t clients_errors;
	uint32_t clients_closed;
} ftp_clients_stats_t;

err_t ftp_client_start(struct tcp_pcb *newpcb);
void ftp_clients_stop(void);
void ftp_client_init(void);

const ftp_clients_stats_t* ftp_clients_stats_get(void);
void ftp_clients_stats_clear(void);

#endif /* FTP_SERVER_FTP_CLIENT_H_ */
