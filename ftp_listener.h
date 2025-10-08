/*
 * ftp_listener.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_LISTENER_H_
#define FTP_SERVER_FTP_LISTENER_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint8_t listeners_active;
	uint8_t listeners_max;
	uint32_t listeners_rejected;
	uint32_t listeners_tcp_stack_error;
	uint32_t listeners_opened;
	uint32_t listeners_closed;
	uint32_t listeners_errors;
} ftp_listener_stats_t;

bool ftp_listener_start(void);
bool ftp_listener_stop(void);
void ftp_listener_init(void);

const ftp_listener_stats_t* ftp_listener_stats_get(void);
void ftp_listener_stats_clear(void);
bool ftp_listener_is_active(void);

#endif /* FTP_SERVER_FTP_LISTENER_H_ */
