/*
 * ftp_activ.c
 *
 *  Created on: Oct 5, 2025
 *      Author: jakubczekaj
 */
#include "ftp_active.h"
#include <stdbool.h>
#include "ftp_config.h"
#include "ftp_data_connection.h"
#include "ftp_cmd.h"

#define FTP_TCP_MAX_IDLE_SEC	10

typedef struct {
	uint8_t index;
	struct tcpip_callback_msg *connect_cb;
	struct tcp_pcb *pcb;
	ip4_addr_t ipaddr;
	uint16_t port;
	uint8_t idle_cnt;
} ftp_active_data_t;

typedef struct {
	ftp_active_data_t active[FTP_NBR_CLIENTS];
	ftp_active_stats_t stats;
	bool inited;
} ftp_active_t;

static ftp_active_t ftp_active = { 0 };

static void ftp_active_clean(ftp_active_data_t *data) {
	data->idle_cnt = 0;
	data->pcb = NULL;
	data->port = 0;
	IP4_ADDR(&(data->ipaddr), 0, 0, 0, 0);
}

static void ftp_active_after_close(ftp_active_data_t *data) {
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP active conn %d disconnected\r\n", data->index);
	if (ftp_active.stats.active_connected) {
		ftp_active.stats.active_connected--;
		ftp_active.stats.active_closed++;
	}
	ftp_active_clean(data);
}

static err_t ftp_active_close(ftp_active_data_t *data) {
	if (tcp_close(data->pcb) == ERR_OK) {
		ftp_active_after_close(data);
		return (ERR_OK);
	} else {
		tcp_abort(data->pcb);
		return (ERR_ABRT);
	}
}

/** Function prototype for tcp connected callback functions. Called when a pcb
 * is connected to the remote side after initiating a connection attempt by
 * calling tcp_connect().
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param tpcb The connection pcb which is connected
 * @param err An unused error code, always ERR_OK currently ;-) @todo!
 *            Only return ERR_ABRT if you have called tcp_abort from within the
 *            callback function!
 *
 * @note When a connection attempt fails, the error callback is currently called!
 */
static err_t ftp_active_connected(void *arg, struct tcp_pcb *tpcb, err_t err) {
	UNUSED(err);
	ftp_active_data_t *data = (ftp_active_data_t*) arg;
	err_t err2 = ftp_data_conn_start(data->index, tpcb, DCM_ACTIVE);
	if (err2 == ERR_OK) {
		DEBUG_PRINT(data->index, "Active data conn OK on connect: %d\r\n", err2);
	} else {
		DEBUG_PRINT(data->index, "Active data conn NOK on connect: %d\r\n", err2);
	}
	ftp_active_after_close(data);
	return (err2);
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
static void ftp_active_err(void *arg, err_t err) {
	UNUSED(err);
	ftp_active_data_t *data = (ftp_active_data_t*) arg;

	ftp_active.stats.active_errors++;
	DEBUG_PRINT(data->index, "Active data conn error on connect: %d\r\n", err);
	ftp_active_after_close(data);
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
static err_t ftp_active_poll(void *arg, struct tcp_pcb *tpcb) {
	ftp_active_data_t *data = (ftp_active_data_t*) arg;

	data->idle_cnt++;
	if (data->idle_cnt >= FTP_TCP_MAX_IDLE_SEC) {
		ftp_active.stats.active_timeouts++;
		return (ftp_active_close(data));
	}
	return (ERR_OK);
}

static void ftp_active_connect_cb(void *ctx) {
	ftp_active_data_t *data = (ftp_active_data_t*) ctx;

	if (data->pcb == NULL) {
		data->pcb = tcp_new();
		if (data->pcb == NULL) {
			ftp_active.stats.active_tcp_stack_error++;
			return;
		}
		if (tcp_bind(data->pcb, IP4_ADDR_ANY, 0) != ERR_OK) {
			ftp_active.stats.active_tcp_stack_error++;
			tcp_close(data->pcb);
			return;
		}
		tcp_arg(data->pcb, data);
		tcp_err(data->pcb, ftp_active_err);
		tcp_poll(data->pcb, ftp_active_poll, 2);
		ftp_active.stats.active_connected++;
		err_t err = tcp_connect(data->pcb, &(data->ipaddr), data->port, ftp_active_connected);
		if (err == ERR_OK) {
			DEBUG_PRINT(index, "Active data conn connecting...\r\n");
		} else {
			DEBUG_PRINT(index, "Error on connecting active data conn: %d\r\n", err);
		}
	} else {
		ftp_active.stats.active_rejected++;
	}
}

err_t ftp_active_connect(uint8_t index, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
	if (ftp_active.active[index].pcb == NULL) {
		IP4_ADDR(&(ftp_active.active[index].ipaddr), a, b, c, d);
		ftp_active.active[index].port = port;
		ftp_active.active[index].pcb = NULL;
		err_t err = tcpip_callbackmsg_trycallback(ftp_active.active[index].connect_cb);
		if (err == ERR_OK) {
			DEBUG_PRINT(index, "Active data conn waits to open...\r\n");
		} else {
			DEBUG_PRINT(index, "Error on starting active data conn: %d\r\n", err);
		}
		return (err);
	} else {
		DEBUG_PRINT(index, "Error on starting active data conn: pcb NOT empty\r\n");
		return (ERR_ARG);
	}
}

void ftp_active_init(void) {
	if (!ftp_active.inited) {
		ftp_active.inited = true;
		for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
			ftp_active.active[i].index = i;
			ftp_active.active[i].connect_cb = tcpip_callbackmsg_new(ftp_active_connect_cb, &(ftp_active.active[i]));
			assert_param(ftp_active.active[i].connect_cb != NULL);
		}
	}
}
