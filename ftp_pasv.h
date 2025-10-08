/*
 * ftp_pasv.h
 *
 *  Created on: Oct 4, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_PASV_H_
#define FTP_SERVER_FTP_PASV_H_

#include <stdint.h>
#include <stdbool.h>
#include "lwip.h"

typedef struct {
	uint8_t listeners_active;
	uint8_t listeners_max;
	uint32_t listeners_rejected;
	uint32_t listeners_tcp_stack_error;
	uint32_t listeners_opened;
	uint32_t listeners_timeouts;
	uint32_t listeners_closed;
	uint32_t listeners_errors;
	uint32_t listeners_data_conn_accepted;
	uint32_t listeners_data_conn_rejected;
} ftp_pasv_listener_stats_t;

err_t ftp_pasv_listener_poll(uint8_t index);
bool ftp_pasv_start(uint8_t index);
bool ftp_pasv_stop(uint8_t index);
void ftp_pasv_listener_stop(uint8_t index);
void ftp_pasv_listeners_stop(void);
void ftp_pasv_init(void);

const ftp_pasv_listener_stats_t* ftp_pasv_listener_stats_get(void);
void ftp_pasv_listener_stats_clear(void);
bool ftp_pasv_listener_is_active(uint8_t index);

#endif /* FTP_SERVER_FTP_PASV_H_ */
