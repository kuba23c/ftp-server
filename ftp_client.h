/*
 * ftp_client.h
 *
 *  Created on: Sep 19, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_CLIENT_H_
#define FTP_SERVER_FTP_CLIENT_H_

#include <stdint.h>
#include "ftp_cmd.h"

typedef enum {
	DCM_NOT_SET,
	DCM_PASSIVE,
	DCM_ACTIVE
} dcm_type;

typedef enum {
	FTP_USER_NONE,
	FTP_USER_USER_NO_PASS,
	FTP_USER_USER_LOGGED_IN
} ftp_user_t;

typedef struct {
	uint8_t clients_connected;
	uint8_t clients_max;
	uint32_t clients_rejected;
	uint32_t clients_accepted;
	uint32_t clients_timeouts;
	uint32_t clients_errors;
	uint32_t clients_closed;
} ftp_clients_stats_t;

ftp_result_t ftp_cmd_resp_send(uint8_t index, const char *fmt, ...);
bool ftp_is_logged_in(uint8_t index);
ftp_user_t ftp_get_user(uint8_t index);
void ftp_set_user(uint8_t index, ftp_user_t user);
bool ftp_is_user_name_ok(char *name);
bool ftp_is_pass_ok(char *pass);
char* ftp_get_path(uint8_t index);
void ftp_set_data_conn_mode(uint8_t index, dcm_type mode);
dcm_type ftp_get_data_conn_mode(uint8_t index);

err_t ftp_client_start(struct tcp_pcb *newpcb);
void ftp_client_stop(uint8_t index);
void ftp_clients_stop(void);
void ftp_clients_init(void);

const ftp_clients_stats_t* ftp_clients_stats_get(void);
void ftp_clients_stats_clear(void);

#endif /* FTP_SERVER_FTP_CLIENT_H_ */
