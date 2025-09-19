/*
 * ftp_client.c
 *
 *  Created on: Sep 19, 2025
 *      Author: jakubczekaj
 */

#include "ftp_client.h"
#include "ftp_config.h"
#include "ftp_cmd.h"
#include "ftp_data.h"

#define FTP_TCP_KEEP_IDLE 	3000
#define FTP_TCP_KEEP_INTVL 	1000
#define FTP_TCP_KEEP_CNT 	3
#define FTP_TCP_MAX_IDLE_SEC	10

typedef struct {
	uint8_t index;
	struct tcp_pcb *client_pcb;
	uint8_t idle_cnt;
} ftp_client_t;

typedef struct {
	ftp_client_t client[FTP_NBR_CLIENTS];
	ftp_clients_stats_t stats;
	bool inited;
} ftp_clients_t;

static ftp_clients_t ftp_clients = { 0 };

static void ftp_client_clean(ftp_client_t *client) {
	memset(client, 0, sizeof(ftp_client_t));
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
static void ftp_client_err(void *arg, err_t err) {
	UNUSED(err);
	ftp_client_t *client = (ftp_client_t*) arg;
	ftp_client_clean(client);
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP %d disconnected\r\n", client->index);
	ftp_clients.stats.clients_errors++;
	if (ftp_clients.stats.clients_connected) {
		ftp_clients.stats.clients_connected--;
		ftp_clients.stats.clients_closed++;
	}
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
static err_t ftp_client_poll(void *arg, struct tcp_pcb *tpcb) {
	ftp_client_t *client = (ftp_client_t*) arg;

	client->idle_cnt++;
	if (client->idle_cnt >= FTP_TCP_MAX_IDLE_SEC) {
		ftp_clients.stats.clients_timeouts++;
		ftp_client_clean(client);
		if (tcp_close(tpcb) != ERR_OK) {
			tcp_abort(tpcb);
			return (ERR_ABRT);
		} else {
			FTP_DISCONNECTED_CALLBACK();
			FTP_LOG_PRINT("FTP %d disconnected\r\n", client->index);
			if (ftp_clients.stats.clients_connected) {
				ftp_clients.stats.clients_connected--;
				ftp_clients.stats.clients_closed++;
			}
		}
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
static err_t ftp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
	ftp_client_t *client = (ftp_client_t*) arg;
	err_t res = ftp_data_sent(client->index, tpcb, len);
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
static err_t ftp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
	ftp_client_t *client = (ftp_client_t*) arg;

	if (err || client->client_pcb != tpcb) {
		ftp_client_clean(client);
		if (p != NULL) {
			pbuf_free(p);
		}
		tcp_abort(tpcb);
		return (ERR_ABRT);
	}
	if (p == NULL) {
		ftp_client_clean(client);
		if (tcp_close(tpcb) != ERR_OK) {
			tcp_abort(tpcb);
			return (ERR_ABRT);
		} else {
			FTP_DISCONNECTED_CALLBACK();
			FTP_LOG_PRINT("FTP %d disconnected\r\n", client->index);
			if (ftp_clients.stats.clients_connected) {
				ftp_clients.stats.clients_connected--;
				ftp_clients.stats.clients_closed++;
			}
		}
		return (ERR_OK);
	}

	client->idle_cnt = 0;
	err_t res = ftp_cmd_handle(client->index, tpcb, p);
	tcp_recved(tpcb, p->tot_len);
	pbuf_free(p);
	return (res);
}

err_t ftp_client_start(struct tcp_pcb *newpcb) {
	err_t result = ERR_ABRT;
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_clients.client[i].client_pcb == NULL) {
			FTP_CONNECTED_CALLBACK();
			FTP_LOG_PRINT("FTP %d connected\r\n", i);
			newpcb->keep_idle = FTP_TCP_KEEP_IDLE;
			newpcb->keep_intvl = FTP_TCP_KEEP_INTVL;
			newpcb->keep_cnt = FTP_TCP_KEEP_CNT;
			tcp_nagle_disable(newpcb);

			ftp_clients.client[i].index = i;
			ftp_clients.client[i].client_pcb = newpcb;
			ftp_clients.client[i].idle_cnt = 0;

			tcp_arg(ftp_clients.client[i].client_pcb, &ftp_clients.client[i]);
			tcp_err(ftp_clients.client[i].client_pcb, ftp_client_err);
			tcp_poll(ftp_clients.client[i].client_pcb, ftp_client_poll, 2);
			tcp_sent(ftp_clients.client[i].client_pcb, ftp_client_sent);
			tcp_recv(ftp_clients.client[i].client_pcb, ftp_client_recv);
			result = ERR_OK;
			break;
		}
	}
	if (result == ERR_ABRT) {
		tcp_abort(newpcb);
		ftp_clients.stats.clients_rejected++;
	} else {
		ftp_clients.stats.clients_accepted++;
		ftp_clients.stats.clients_connected++;
	}
	return (result);
}

void ftp_clients_stop(void) {
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_clients.client[i].client_pcb) {
			FTP_DISCONNECTED_CALLBACK();
			FTP_LOG_PRINT("FTP %d disconnected\r\n", i);
			if (tcp_close(ftp_clients.client[i].client_pcb) == ERR_OK) {
				if (ftp_clients.stats.clients_connected) {
					ftp_clients.stats.clients_connected--;
				}
				ftp_clients.stats.clients_closed++;
			} else {
				tcp_abort(ftp_clients.client[i].client_pcb);
			}
		}
	}
}

void ftp_clients_init(void) {
	if (!ftp_clients.inited) {
		ftp_clients.inited = true;
		ftp_clients.stats.clients_max = FTP_NBR_CLIENTS;
	}
}

/**
 * @brief get stats of ftp clients
 * @return pointer to stats
 */
const ftp_clients_stats_t* ftp_clients_stats_get(void) {
	return (&ftp_clients.stats);
}

/**
 * @brief clear ftp clients stats
 * Do it only when ftp listener is NOT active
 */
void ftp_clients_stats_clear(void) {
	memset(&ftp_clients.stats, 0, sizeof(ftp_clients_stats_t));
}
