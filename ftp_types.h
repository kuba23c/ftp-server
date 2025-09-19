/*
 * ftp_types.h
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_TYPES_H_
#define FTP_SERVER_FTP_TYPES_H_

#include <stdint.h>

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
	uint32_t clients_denied;
	uint32_t files_send_successfully;
	uint32_t files_send_failed;
	uint32_t files_received_successfully;
	uint32_t files_received_failed;
} ftp_stats_t;

#endif /* FTP_SERVER_FTP_TYPES_H_ */
