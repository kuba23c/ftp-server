/*
 * ftp_listener.c
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#include "ftp_listener.h"
#include "lwip.h"
#include "ftp_config.h"
#include "ftp_client.h"

typedef struct {
	struct tcp_pcb *listener_pcb;
	struct tcpip_callback_msg *create_listener;
	struct tcpip_callback_msg *delete_listener;
	ftp_listener_stats_t stats;
	bool inited;
} ftp_listener_t;

static ftp_listener_t ftp_listener = { 0 };

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
static void ftp_listener_err(void *arg, err_t err) {
	UNUSED(err);
	struct tcp_pcb **ptcp_pcb = (struct tcp_pcb**) arg;
	*ptcp_pcb = NULL;
	ftp_listener.stats.listeners_errors++;
	if (ftp_listener.stats.listeners_active) {
		ftp_listener.stats.listeners_active--;
	}
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
static err_t ftp_listener_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
	UNUSED(arg);
	UNUSED(err);
	return (ftp_client_start(newpcb));
}

static void ftp_listener_start(void *ctx) {
	UNUSED(ctx);
	if (ftp_listener.listener_pcb == NULL) {
		ftp_listener.listener_pcb = tcp_new();
		if (ftp_listener.listener_pcb == NULL) {
			ftp_listener.stats.listeners_tcp_stack_error++;
			return;
		}
		if (tcp_bind(ftp_listener.listener_pcb, IP4_ADDR_ANY, FTP_SERVER_PORT) != ERR_OK) {
			ftp_listener.stats.listeners_tcp_stack_error++;
			tcp_close(ftp_listener.listener_pcb);
			return;
		}
		tcp_err(ftp_listener.listener_pcb, ftp_listener_err);
		ftp_listener.listener_pcb = tcp_listen(ftp_listener.listener_pcb);
		if (ftp_listener.listener_pcb == NULL) {
			ftp_listener.stats.listeners_tcp_stack_error++;
			return;
		}
		tcp_arg(ftp_listener.listener_pcb, &ftp_listener.listener_pcb);
		tcp_accept(ftp_listener.listener_pcb, ftp_listener_accept);
		ftp_listener.stats.listeners_active++;
		ftp_listener.stats.listeners_opened++;
	} else {
		ftp_listener.stats.listeners_rejected++;
	}
}

static void ftp_listener_stop(void *ctx) {
	UNUSED(ctx);
	if (ftp_listener.listener_pcb) {
		if (tcp_close(ftp_listener.listener_pcb) == ERR_OK) {
			if (ftp_listener.stats.listeners_active) {
				ftp_listener.stats.listeners_active--;
			}
			ftp_listener.stats.listeners_closed++;
		} else {
			tcp_abort(ftp_listener.listener_pcb);
		}
	}
	ftp_clients_stop();
}

/**
 * @brief Init ftp server
 * call only once
 */
void ftp_init(void) {
	ftp_client_init();
	if (!ftp_listener.inited) {
		ftp_listener.inited = true;
		ftp_listener.stats.listeners_max = 1;
		ftp_listener.create_listener = tcpip_callbackmsg_new(ftp_listener_start, NULL);
		assert_param(ftp_listener.create_listener != NULL);
		ftp_listener.delete_listener = tcpip_callbackmsg_new(ftp_listener_stop, NULL);
		assert_param(ftp_listener.delete_listener != NULL);
	}
}

/**
 * @brief Start ftp server
 */
bool ftp_start(void) {
	return (tcpip_callbackmsg_trycallback(ftp_listener.create_listener) == ERR_OK);
}

/**
 * @brief Stop ftp server
 */
bool ftp_stop(void) {
	return (tcpip_callbackmsg_trycallback(ftp_listener.delete_listener) == ERR_OK);
}

/**
 * @brief get stats of ftp listener
 * @return pointer to stats
 */
const ftp_listener_stats_t* ftp_listener_stats_get(void) {
	return (&ftp_listener.stats);
}

/**
 * @brief clear ftp listener stats
 * Do it only when ftp listener is NOT active
 */
void ftp_listener_stats_clear(void) {
	memset(&ftp_listener.stats, 0, sizeof(ftp_listener_stats_t));
}

/**
 * @brief check if ftp listener is active
 * It is NOT set in ftp_start but some time later when lwip stack will pick up a message,
 * so you have to wait a litte after ftp_start call
 * @return 	true - ftp listener is active
 * 			false - ftp listener is NOT active
 */
bool ftp_listener_is_active(void) {
	if (ftp_listener.stats.listeners_active) {
		return (true);
	} else {
		return (false);
	}
}

