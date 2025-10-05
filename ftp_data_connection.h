/*
 * ftp_data_connection.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_DATA_CONNECTION_H_
#define FTP_SERVER_FTP_DATA_CONNECTION_H_

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
} ftp_data_conns_stats_t;

err_t ftp_data_conn_start(uint8_t index, struct tcp_pcb *newpcb);
void ftp_data_conn_stop(uint8_t index);
void ftp_data_conns_stop(void);
void ftp_data_conn_init(void);

const ftp_data_conns_stats_t* ftp_data_conn_stats_get(void);
void ftp_data_conn_stats_clear(void);

#endif /* FTP_SERVER_FTP_DATA_CONNECTION_H_ */
