/*
 * ftp_data.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_DATA_H_
#define FTP_SERVER_FTP_DATA_H_

#include <stdint.h>
#include <stdbool.h>
#include "ftp_cmd.h"

#define FTP_CWD_SIZE				_MAX_LFN + 8

typedef enum {
	DCM_NOT_SET,
	DCM_PASSIVE,
	DCM_ACTIVE
} dcm_type;

typedef struct __PACKED {
	uint8_t index;
	struct tcp_pcb *tpcb;
} ftp_data_msg_client_t;

typedef struct __PACKED {
	ftp_data_msg_client_t client;
	struct pbuf *p;
} ftp_data_msg_t;

ftp_result_t ftp_send(const ftp_cmd_msg_client_t *const client, const char *fmt, ...);
err_t ftp_data_sent(const ftp_cmd_msg_client_t *const client, uint16_t len);
bool ftp_is_logged_in(const ftp_cmd_msg_client_t *const client);
char* ftp_get_path(const ftp_cmd_msg_client_t *const client);
void ftp_set_connection_mode(const ftp_cmd_msg_client_t *const client, dcm_type data_conn_mode);
dcm_type ftp_get_connection_mode(const ftp_cmd_msg_client_t *const client);
err_t ftp_data_handle(const ftp_data_msg_t *const msg);
void ftp_data_init(void);

#endif /* FTP_SERVER_FTP_DATA_H_ */
