/*
 * ftp_data_connection.c
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#include "ftp_data_connection.h"
#include "ftp_config.h"
#include "ftp_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "ftp_client.h"
#include "lwip.h"

#define FTP_TCP_KEEP_IDLE 	3000
#define FTP_TCP_KEEP_INTVL 	1000
#define FTP_TCP_KEEP_CNT 	3
#define FTP_TCP_MAX_IDLE_SEC	5

#define FTP_BUF_SIZE_MIN 			TCP_SND_BUF
#define FTP_BUF_SIZE 				(FTP_BUF_SIZE_MIN * FTP_BUF_SIZE_MULT)

typedef struct {
	uint8_t index;
	struct tcp_pcb *client_pcb;
	uint8_t idle_cnt;

	char *ftp_buff;
	lwrb_t lwrb;
	struct tcpip_callback_msg *cb;
	SemaphoreHandle_t mutex;

	struct tcpip_callback_msg *close_cb;
} ftp_data_conn_t;

typedef struct {
	ftp_data_conn_t client[FTP_NBR_CLIENTS];
	ftp_data_conns_stats_t stats;
	bool inited;
} ftp_data_conns_t;

static ftp_data_conns_t ftp_data_conns = { 0 };
FTP_STRUCT_MEM_SECTION(ALIGN_32BYTES(static char ftp_buffs[FTP_NBR_CLIENTS][FTP_BUF_SIZE])) = {0};

static void ftp_data_send_cb(void *ctx) {
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) ctx;
	lwrb_sz_t len = lwrb_get_linear_block_read_length(&(data_conn->lwrb));
	void *addr = lwrb_get_linear_block_read_address(&(data_conn->lwrb));
	if (len > TCP_SND_BUF) {
		len = TCP_SND_BUF;
	}
	if (data_conn->client_pcb != NULL && len != 0) {
		tcp_write(data_conn->client_pcb, addr, len, 0);
	}
}

ftp_result_t ftp_data_send(uint8_t index) {
	err_t err = tcpip_callbackmsg_trycallback(ftp_data_conns.client[index].cb);
	if (err == ERR_OK) {
		DEBUG_PRINT(index, "DATA waits to send...\r\n");
		return (FTP_RES_OK);
	} else {
		DEBUG_PRINT(index, "Error on sending DATA: %d\r\n", err);
		return (FTP_RES_ERROR);
	}
}

ftp_result_t ftp_data_lock(uint8_t index) {
	if (xSemaphoreTake(ftp_data_conns.client[index].mutex, 1000) == pdTRUE) {
		return (FTP_RES_OK);
	} else {
		return (FTP_RES_TIMEOUT);
	}
}

void ftp_data_unlock(uint8_t index) {
	xSemaphoreGive(ftp_data_conns.client[index].mutex);
}

lwrb_t* ftp_data_get_lwrb(uint8_t index) {
	return (&(ftp_data_conns.client[index].lwrb));
}

static void ftp_data_conn_clean(ftp_data_conn_t *data_conn) {
	data_conn->client_pcb = NULL;
	data_conn->idle_cnt = 0;
	lwrb_reset(&(data_conn->lwrb));
	xSemaphoreGive(data_conn->mutex);
}

static void ftp_data_conn_after_close(ftp_data_conn_t *data_conn) {
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP data_conn %d disconnected\r\n", data_conn->index);
	ftp_data_msg_t msg = { 0 };
	msg.msg_type = FTP_DATA_MSG_STOP;
	msg.index = data_conn->index;
	msg.data.stop = NULL;
	ftp_data_handle(&msg);
	if (ftp_data_conns.stats.clients_connected) {
		ftp_data_conns.stats.clients_connected--;
		ftp_data_conns.stats.clients_closed++;
	}
	ftp_data_conn_clean(data_conn);
}

static err_t ftp_data_conn_close(ftp_data_conn_t *data_conn) {
	if (xSemaphoreTake(data_conn->mutex, 1000) == pdTRUE) {
		if (tcp_close(data_conn->client_pcb) == ERR_OK) {
			ftp_data_conn_after_close(data_conn);
			xSemaphoreGive(data_conn->mutex);
			return (ERR_OK);
		} else {
			tcp_abort(data_conn->client_pcb);
			xSemaphoreGive(data_conn->mutex);
			return (ERR_ABRT);
		}
	} else {
		DEBUG_PRINT(data_conn->index, "Timeout on DATA mutex take, on clear\r\n");
		tcp_abort(data_conn->client_pcb);
		xSemaphoreGive(data_conn->mutex);
		return (ERR_ABRT);
	}
}

static void ftp_data_close_cb(void *ctx) {
	ftp_data_conn_t *data_conn = (ftp_data_conn_t*) ctx;
	if (data_conn->client_pcb) {
		ftp_data_conn_close(data_conn);
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
	ftp_client_refresh(data_conn->index);

	lwrb_skip(&(data_conn->lwrb), len);
	uint32_t available_len = lwrb_get_full(&(data_conn->lwrb));

	if (available_len <= TCP_SND_BUF) {
		if (available_len != 0) {
			ftp_data_send(data_conn->index);
		}
		ftp_data_msg_t msg = { 0 };
		msg.msg_type = FTP_DATA_MSG_SENT;
		msg.index = data_conn->index;
		msg.data.sent_len = len;
		if (ftp_data_handle(&msg) == ERR_OK) {
			return (ERR_OK);
		} else {
			return (ftp_data_conn_close(data_conn));
		}
	} else {
		ftp_data_send(data_conn->index);
	}

	return (ERR_OK);
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
	ftp_data_msg_t msg = { 0 };
	if (p == NULL) {
		msg.msg_type = FTP_DATA_MSG_RECV;
		msg.index = data_conn->index;
		msg.data.recv_p = NULL;
		ftp_data_handle(&msg);
		return (ftp_data_conn_close(data_conn));
	}

	data_conn->idle_cnt = 0;
	ftp_client_refresh(data_conn->index);
	msg.msg_type = FTP_DATA_MSG_RECV;
	msg.index = data_conn->index;
	msg.data.recv_p = p;
	pbuf_ref(p);
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
		ftp_cmd_resp_send(index, "425 Can't create connection\r\n");
	} else {
		ftp_data_conns.stats.clients_accepted++;
		ftp_data_conns.stats.clients_connected++;
		ftp_cmd_resp_send(index, "150 Accepted data connection\r\n");
		ftp_data_msg_t msg = { 0 };
		msg.msg_type = FTP_DATA_MSG_CONNECTED;
		msg.index = index;
		msg.data.stop = NULL;
		ftp_data_handle(&msg);
	}
	return (result);
}

void ftp_data_conn_stop(uint8_t index) {
	if (ftp_data_conns.client[index].client_pcb) {
		ftp_data_conn_close(&(ftp_data_conns.client[index]));
	}
}

void ftp_data_conns_stop(void) {
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_data_conns.client[i].client_pcb) {
			ftp_data_conn_close(&(ftp_data_conns.client[i]));
		}
	}
}

err_t ftp_data_conn_stop_ex(uint8_t index) {
	return (tcpip_callbackmsg_trycallback(ftp_data_conns.client[index].close_cb));
}

void ftp_data_conns_init(void) {
	if (!ftp_data_conns.inited) {
		ftp_data_conns.inited = true;
		ftp_data_conns.stats.clients_max = FTP_NBR_CLIENTS;
		for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
			ftp_data_conns.client[i].index = i;
			ftp_data_conns.client[i].idle_cnt = 0;
			ftp_data_conns.client[i].ftp_buff = ftp_buffs[i];
			assert_param(lwrb_init(&(ftp_data_conns.client[i].lwrb), ftp_data_conns.client[i].ftp_buff, FTP_BUF_SIZE + 1) == 1);
			ftp_data_conns.client[i].mutex = xSemaphoreCreateMutex();
			assert_param(ftp_data_conns.client[i].mutex != NULL);
			xSemaphoreGive(ftp_data_conns.client[i].mutex);
			ftp_data_conns.client[i].cb = tcpip_callbackmsg_new(ftp_data_send_cb, &(ftp_data_conns.client[i]));
			assert_param(ftp_data_conns.client[i].cb != NULL);
			ftp_data_conns.client[i].close_cb = tcpip_callbackmsg_new(ftp_data_close_cb, &(ftp_data_conns.client[i]));
			assert_param(ftp_data_conns.client[i].close_cb != NULL);
		}
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
