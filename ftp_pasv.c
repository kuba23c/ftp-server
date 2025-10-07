/*
 * ftp_pasv.c
 *
 *  Created on: Oct 4, 2025
 *      Author: jakubczekaj
 */
#include "ftp_pasv.h"
#include "lwip.h"
#include "ftp_config.h"
#include "ftp_client.h"
#include "ftp_data_connection.h"

#define FTP_TCP_MAX_IDLE_SEC	4
#define PORT_INCREMENT_OFFSET	25 // used for a bugfix which works around ports which are already in use (from a previous connection)

typedef struct {
	uint8_t index;
	uint8_t idle_cnt;
	uint8_t data_port_incremented;
	struct tcp_pcb *listener_pcb;
	struct tcpip_callback_msg *create_listener;
	struct tcpip_callback_msg *delete_listener;
} ftp_pasv_listener_data_t;

typedef struct {
	ftp_pasv_listener_data_t data[FTP_NBR_CLIENTS];
	ftp_pasv_listener_stats_t stats;
	bool inited;
} ftp_pasv_listener_t;

static ftp_pasv_listener_t ftp_pasv_listener = { 0 };

static void ftp_pasv_data_clean(ftp_pasv_listener_data_t *data) {
	data->idle_cnt = 0;
	data->listener_pcb = NULL;
}

static void ftp_pasv_data_after_close(ftp_pasv_listener_data_t *data) {
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP pasv listener %d disconnected\r\n", data->index);
	if (ftp_pasv_listener.stats.listeners_active) {
		ftp_pasv_listener.stats.listeners_active--;
		ftp_pasv_listener.stats.listeners_closed++;
	}
	ftp_pasv_data_clean(data);
}

static err_t ftp_pasv_data_close(ftp_pasv_listener_data_t *data) {
	if (tcp_close(data->listener_pcb) == ERR_OK) {
		ftp_pasv_data_after_close(data);
		return (ERR_OK);
	} else {
		tcp_abort(data->listener_pcb);
		return (ERR_ABRT);
	}
}

/** Function prototype for tcp error callback functions. Called when the pcb
 * receives a RST or is unexpectedly closed for any other reason.
 *
 * @note The corresponding pcb is already freed when this callback is called!
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param err Error code to indicate why the pcb has been closed
 *            ERR_ABRT: aborted through tcp_abort or by a TCP timer
 *            ERR_RST: the connection was reset by the remote host
 */
static void ftp_pasv_listener_err(void *arg, err_t err) {
	UNUSED(err);
	ftp_pasv_listener_data_t *data = (ftp_pasv_listener_data_t*) arg;
	ftp_pasv_listener.stats.listeners_errors++;
	ftp_pasv_data_after_close(data);
}

/** Function prototype for tcp accept callback functions. Called when a new
 * connection can be accepted on a listening pcb.
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param newpcb The new connection pcb
 * @param err An error code if there has been an error accepting.
 *            Only return ERR_ABRT if you have called tcp_abort from within the
 *            callback function!
 */
static err_t ftp_pasv_listener_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
	ftp_pasv_listener_data_t *data = (ftp_pasv_listener_data_t*) arg;
	UNUSED(err);

	if (data->listener_pcb == NULL) {
		ftp_pasv_listener.stats.listeners_data_conn_rejected++;
	} else if (ftp_data_conn_start(data->index, newpcb) == ERR_OK) {
		ftp_pasv_listener.stats.listeners_data_conn_accepted++;
		return (ftp_pasv_data_close(data));
	} else {
		ftp_pasv_listener.stats.listeners_data_conn_rejected++;
	}

	return (ERR_OK);
}

/** Function prototype for tcp poll callback functions. Called periodically as
 * specified by @see tcp_poll.
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param tpcb tcp pcb
 * @return ERR_OK: try to send some data by calling tcp_output
 *            Only return ERR_ABRT if you have called tcp_abort from within the
 *            callback function!
 */
static err_t ftp_listener_poll(void *arg, struct tcp_pcb *tpcb) {
	ftp_pasv_listener_data_t *data = (ftp_pasv_listener_data_t*) arg;

	data->idle_cnt++;
	if (data->idle_cnt >= FTP_TCP_MAX_IDLE_SEC) {
		ftp_pasv_listener.stats.listeners_timeouts++;
		ftp_cmd_resp_send(data->index, "425 Can't create connection\r\n");
		return (ftp_pasv_data_close(data));
	}
	return (ERR_OK);
}

static void ftp_pasv_listener_start(void *ctx) {
	ftp_pasv_listener_data_t *data = (ftp_pasv_listener_data_t*) ctx;

	if (data->listener_pcb == NULL) {
		data->listener_pcb = tcp_new();
		if (data->listener_pcb == NULL) {
			ftp_pasv_listener.stats.listeners_tcp_stack_error++;
			ftp_cmd_resp_send(data->index, "425 Can't set connection management to passive\r\n");
			return;
		}
		data->data_port_incremented = (data->data_port_incremented + 1) % PORT_INCREMENT_OFFSET;
		if (tcp_bind(data->listener_pcb, IP4_ADDR_ANY, FTP_DATA_PORT + data->data_port_incremented + (data->index * PORT_INCREMENT_OFFSET)) != ERR_OK) {
			ftp_pasv_listener.stats.listeners_tcp_stack_error++;
			tcp_close(data->listener_pcb);
			ftp_cmd_resp_send(data->index, "425 Can't set connection management to passive\r\n");
			return;
		}
		tcp_err(data->listener_pcb, ftp_pasv_listener_err);
		data->listener_pcb = tcp_listen(data->listener_pcb);
		if (data->listener_pcb == NULL) {
			ftp_pasv_listener.stats.listeners_tcp_stack_error++;
			ftp_cmd_resp_send(data->index, "425 Can't set connection management to passive\r\n");
			return;
		}
		tcp_arg(data->listener_pcb, &data->listener_pcb);
		tcp_accept(data->listener_pcb, ftp_pasv_listener_accept);
		data->idle_cnt = 0;
		tcp_poll(data->listener_pcb, ftp_listener_poll, 2);
		ftp_pasv_listener.stats.listeners_active++;
		ftp_pasv_listener.stats.listeners_opened++;

		ftp_cmd_resp_send(data->index, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).\r\n", ip4_addr1(&(data->listener_pcb->local_ip)),
				ip4_addr2(&(data->listener_pcb->local_ip)), ip4_addr3(&(data->listener_pcb->local_ip)), ip4_addr3(&(data->listener_pcb->local_ip)),
				data->listener_pcb->local_port >> 8, data->listener_pcb->local_port & 255);
		ftp_set_data_conn_mode(data->index, DCM_PASSIVE);
	} else {
		ftp_pasv_listener.stats.listeners_rejected++;
		ftp_cmd_resp_send(data->index, "425 Can't set connection management to passive\r\n");
	}
}

static void ftp_pasv_listener_stop(void *ctx) {
	ftp_pasv_listener_data_t *data = (ftp_pasv_listener_data_t*) ctx;

	uint8_t index = data->index;
	if (data->listener_pcb) {
		ftp_pasv_data_close(data);
	}
	ftp_data_conn_stop(index);
}

void ftp_pasv_listeners_stop(void) {
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_pasv_listener.data[i].listener_pcb) {
			ftp_pasv_data_close(&(ftp_pasv_listener.data[i]));
		}
	}
}

/**
 * @brief Init ftp server
 * call only once
 */
void ftp_pasv_init(void) {
	if (!ftp_pasv_listener.inited) {
		ftp_pasv_listener.inited = true;
		ftp_pasv_listener.stats.listeners_max = FTP_NBR_CLIENTS;
		for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
			ftp_pasv_listener.data[i].index = i;
			ftp_pasv_listener.data[i].create_listener = tcpip_callbackmsg_new(ftp_pasv_listener_start, &(ftp_pasv_listener.data[i]));
			assert_param(ftp_pasv_listener.data[i].create_listener != NULL);
			ftp_pasv_listener.data[i].delete_listener = tcpip_callbackmsg_new(ftp_pasv_listener_stop, &(ftp_pasv_listener.data[i]));
			assert_param(ftp_pasv_listener.data[i].delete_listener != NULL);
		}
	}
}

/**
 * @brief Start ftp server
 */
bool ftp_pasv_start(uint8_t index) {
	return (tcpip_callbackmsg_trycallback(ftp_pasv_listener.data[index].create_listener) == ERR_OK);
}

/**
 * @brief Stop ftp server
 */
bool ftp_pasv_stop(uint8_t index) {
	return (tcpip_callbackmsg_trycallback(ftp_pasv_listener.data[index].delete_listener) == ERR_OK);
}

/**
 * @brief get stats of ftp listener
 * @return pointer to stats
 */
const ftp_pasv_listener_stats_t* ftp_pasv_listener_stats_get(void) {
	return (&ftp_pasv_listener.stats);
}

/**
 * @brief clear ftp listener stats
 * Do it only when ftp listener is NOT active
 */
void ftp_pasv_listener_stats_clear(void) {
	memset(&ftp_pasv_listener.stats, 0, sizeof(ftp_pasv_listener_stats_t));
}

/**
 * @brief check if ftp listener is active
 * It is NOT set in ftp_start but some time later when lwip stack will pick up a message,
 * so you have to wait a litte after ftp_start call
 * @return 	true - ftp listener is active
 * 			false - ftp listener is NOT active
 */
bool ftp_pasv_listener_is_active(uint8_t index) {
	if (ftp_pasv_listener.data[index].listener_pcb != NULL) {
		return (true);
	} else {
		return (false);
	}
}
