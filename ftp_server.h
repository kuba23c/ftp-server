/*
 * ftp_server.h
 *
 *  Created on: Aug 31, 2020
 *      Author: Sande
 */

#ifndef _FTP_SERVER_H_
#define _FTP_SERVER_H_

typedef enum {
	FTP_IDLE,
	FTP_STARTING,
	FTP_RUNNING,
	FTP_STOPPING,
	FTP_ERROR_STOPPING,
	FTP_ERROR
} ftp_status_t;

typedef enum {
	FTP_ERROR_SERVER_NETCONN_NEW,
	FTP_ERROR_PORT_IS_ZERO,
	FTP_ERROR_BIND_TO_PORT,
	FTP_ERROR_SERVER_NETCONN_LISTEN,
	FTP_ERROR_SERVER_NETCONN_DELETE,
	FTP_ERROR_CLIENT_NETCONN_WRITE,
	FTP_ERROR_CLIENT_NETCONN_DELETE,
	FTP_ERROR_NOT_ALL_TASK_DISABLED,
	FTP_ERROR_LISTEN_DATA_NETCONN_NEW,
	FTP_ERROR_LISTEN_DATA_NETCONN_BIND,
	FTP_ERROR_LISTEN_DATA_NETCONN_LISTEN,
	FTP_ERROR_LISTEN_DATA_NETCONN_CLOSE,
	FTP_ERROR_LISTEN_DATA_NETCONN_DELETE,
	FTP_ERROR_DATA_NETCONN_NEW,
	FTP_ERROR_DATA_NETCONN_BIND,
	FTP_ERROR_DATA_NETCONN_CLOSE,
	FTP_ERROR_DATA_NETCONN_DELETE,
} ftp_error_t;

typedef struct {
	uint8_t clients_active;
	uint8_t clients_max;
	uint32_t clients_connected;
	uint32_t clients_disconnected;
	uint32_t files_send_successfully;
	uint32_t files_send_faild;
	uint32_t files_received_successfully;
	uint32_t files_received_faild;
} ftp_stats_t;

void ftp_set_username(const char *name);
void ftp_set_password(const char *pass);
void ftp_set_port(uint16_t port);
uint16_t ftp_get_port(void);
ftp_status_t ftp_get_status(void);
uint32_t ftp_get_errors(void);

void ftp_init(void);
void ftp_start(void);
void ftp_stop(void);
void ftp_clear_errors(void);
const ftp_stats_t* ftp_get_stats(void);

#endif /* ETH_FTP_FTP_SERVER_H_ */
