/*
 * ftp.c
 *
 *  Created on: Oct 8, 2025
 *      Author: jakubczekaj
 */

#include "ftp.h"
#include "ftp_config.h"
#include "ftp_listener.h"
#include "ftp_client.h"
#include "ftp_cmd.h"
#include "ftp_pasv.h"
#include "ftp_active.h"
#include "ftp_data_connection.h"
#include "ftp_data.h"

/**
 * @brief Start ftp server
 */
bool ftp_start(void) {
	return (ftp_listener_start());
}

/**
 * @brief Stop ftp server
 */
bool ftp_stop(void) {
	return (ftp_listener_stop());
}

/**
 * @brief Init ftp server
 * call only once
 */
void ftp_init(void) {
	assert_param(TCPIP_MBOX_SIZE > (6 + 2 + FTP_NBR_CLIENTS * 6));
	ftp_data_init();
	ftp_data_conns_init();
	ftp_active_init();
	ftp_pasv_init();
	ftp_cmd_init();
	ftp_clients_init();
	ftp_listener_init();
}
