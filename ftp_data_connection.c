/*
 * ftp_data_connection.c
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#include "ftp_data_connection.h"
#include "ftp_config.h"
#include "ftp_data.h"

#define FTP_TCP_KEEP_IDLE 	3000
#define FTP_TCP_KEEP_INTVL 	1000
#define FTP_TCP_KEEP_CNT 	3
#define FTP_TCP_MAX_IDLE_SEC	5

typedef struct {
	uint8_t index;
	struct tcp_pcb *client_pcb;
	uint8_t idle_cnt;
} ftp_data_conn_t;

typedef struct {
	ftp_data_conn_t client[FTP_NBR_CLIENTS];
	ftp_data_conns_stats_t stats;
	bool inited;
} ftp_data_conns_t;

static ftp_data_conns_t ftp_data_conns = { 0 };

static void ftp_data_conn_clean(ftp_data_conn_t *data_conn) {
	memset(data_conn, 0, sizeof(ftp_data_conn_t));
}

static void ftp_data_conn_after_close(ftp_data_conn_t *data_conn) {
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP data_conn %d disconnected\r\n", data_conn->index);
	ftp_data_msg_t msg = { .client = { .index = data_conn->index, .tpcb = NULL }, .p = NULL };
	ftp_data_handle(&msg);
	if (ftp_data_conns.stats.clients_connected) {
		ftp_data_conns.stats.clients_connected--;
		ftp_data_conns.stats.clients_closed++;
	}
	ftp_data_conn_clean(data_conn);
}

static err_t ftp_data_conn_close(ftp_data_conn_t *data_conn) {
	if (tcp_close(data_conn->client_pcb) == ERR_OK) {
		ftp_data_conn_after_close(data_conn);
		return (ERR_OK);
	} else {
		tcp_abort(data_conn->client_pcb);
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
static void ftp_data_conn_err(void *arg, err_t err) {
	UNUSED(err);
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) arg;
	ftp_data_conns.stats.clients_errors++;
	ftp_data_conn_after_close(data_conn);
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
static err_t ftp_data_conn_poll(void *arg, struct tcp_pcb *tpcb) {
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) arg;

	data_conn->idle_cnt++;
	if (data_conn->idle_cnt >= FTP_TCP_MAX_IDLE_SEC) {
		ftp_data_conns.stats.clients_timeouts++;
		return (ftp_data_conn_close(data_conn));
	}
	return (ERR_OK);
}

/** Function prototype for tcp sent callback functions. Called when sent data has
 * been acknowledged by the remote side. Use it to free corresponding resources.
 * This also means that the pcb has now space available to send new data.
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param tpcb The connection pcb for which data has been acknowledged
 * @param len The amount of bytes acknowledged
 * @return ERR_OK: try to send some data by calling tcp_output
 *            Only return ERR_ABRT if you have called tcp_abort from within the
 *            callback function!
 */
static err_t ftp_data_conn_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) arg;
	data_conn->idle_cnt = 0;
	ftp_cmd_msg_client_t msg_client = { .index = data_conn->index, .tpcb = tpcb };
	err_t res = ftp_data_sent(&msg_client, len);
	return (res);
}

/** Function prototype for tcp receive callback functions. Called when data has
 * been received.
 *
 * @param arg Additional argument to pass to the callback function (@see tcp_arg())
 * @param tpcb The connection pcb which received data
 * @param p The received data (or NULL when the connection has been closed!)
 * @param err An error code if there has been an error receiving
 *            Only return ERR_ABRT if you have called tcp_abort from within the
 *            callback function!
 */
static err_t ftp_data_conn_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) arg;

	if (err || data_conn->client_pcb != tpcb) {
		if (p != NULL) {
			pbuf_free(p);
		}
		tcp_abort(tpcb);
		return (ERR_ABRT);
	}
	if (p == NULL) {
		return (ftp_data_conn_close(data_conn));
	}

	data_conn->idle_cnt = 0;
	ftp_data_msg_t msg = { .client = { .index = data_conn->index, .tpcb = tpcb }, .p = p };
	err_t res = ftp_data_handle(&msg);
	tcp_recved(tpcb, p->tot_len);
	pbuf_free(p);
	return (res);
}

err_t ftp_data_conn_start(uint8_t index, struct tcp_pcb *newpcb) {
	err_t result = ERR_ABRT;

	if (ftp_data_conns.client[index].client_pcb == NULL) {
		FTP_CONNECTED_CALLBACK();
		FTP_LOG_PRINT("FTP data_conn %d connected\r\n", index);
		newpcb->keep_idle = FTP_TCP_KEEP_IDLE;
		newpcb->keep_intvl = FTP_TCP_KEEP_INTVL;
		newpcb->keep_cnt = FTP_TCP_KEEP_CNT;
		tcp_nagle_disable(newpcb);

		ftp_data_conns.client[index].index = index;
		ftp_data_conns.client[index].client_pcb = newpcb;
		ftp_data_conns.client[index].idle_cnt = 0;

		tcp_arg(ftp_data_conns.client[index].client_pcb, &ftp_data_conns.client[index]);
		tcp_err(ftp_data_conns.client[index].client_pcb, ftp_data_conn_err);
		tcp_poll(ftp_data_conns.client[index].client_pcb, ftp_data_conn_poll, 2);
		tcp_sent(ftp_data_conns.client[index].client_pcb, ftp_data_conn_sent);
		tcp_recv(ftp_data_conns.client[index].client_pcb, ftp_data_conn_recv);
		result = ERR_OK;
	}
	if (result == ERR_ABRT) {
		tcp_abort(newpcb);
		ftp_data_conns.stats.clients_rejected++;
	} else {
		ftp_data_conns.stats.clients_accepted++;
		ftp_data_conns.stats.clients_connected++;
	}
	return (result);
}

void ftp_data_conn_stop(uint8_t index) {
	if (ftp_data_conns.client[index].client_pcb) {
		ftp_data_conn_close(ftp_data_conns.client[index]);
	}
}

void ftp_data_conns_stop(void) {
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_data_conns.client[i].client_pcb) {
			ftp_data_conn_close(ftp_data_conns.client[i]);
		}
	}
}

void ftp_data_conns_init(void) {
	if (!ftp_data_conns.inited) {
		ftp_data_conns.inited = true;
		ftp_data_conns.stats.clients_max = FTP_NBR_CLIENTS;
	}
}

/**
 * @brief get stats of ftp clients
 * @return pointer to stats
 */
const ftp_data_conns_stats_t* ftp_data_conns_stats_get(void) {
	return (&ftp_data_conns.stats);
}

/**
 * @brief clear ftp clients stats
 * Do it only when ftp listener is NOT active
 */
void ftp_data_conns_stats_clear(void) {
	memset(&ftp_data_conns.stats, 0, sizeof(ftp_data_conns_stats_t));
}
