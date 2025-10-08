/*
 * ftp_client.c
 *
 *  Created on: Sep 19, 2025
 *      Author: jakubczekaj
 */

#include "ftp_client.h"
#include "ftp_config.h"
#include "ftp_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdarg.h>
#include "ftp_pasv.h"

#define FTP_TCP_KEEP_IDLE 		3000
#define FTP_TCP_KEEP_INTVL 		1000
#define FTP_TCP_KEEP_CNT 		3
#define FTP_TCP_MAX_IDLE_SEC	5
#define FTP_TCP_SENT_MAX_IDLE_SEC	3
#define FTP_VERSION				"2020-08-20"
#define FTP_CMD_BUFF_SIZE		(_MAX_LFN * 2)
#define FTP_IS_LOGGED_IN(p_ftp)		(p_ftp->user == FTP_USER_USER_LOGGED_IN)

typedef struct {
	uint8_t index;
	struct tcp_pcb *client_pcb;
	uint8_t idle_cnt;

	char *buff;
	struct tcpip_callback_msg *cb;
	struct tcpip_callback_msg *close_cb;
	uint32_t len;
	uint8_t sent_idle_cnt;
	SemaphoreHandle_t buff_available_sem;
	SemaphoreHandle_t mutex;

	char path[FTP_CWD_SIZE];
	ftp_user_t user;
	dcm_type data_conn_mode;
	bool locked;
} ftp_client_t;

typedef struct {
	ftp_client_t client[FTP_NBR_CLIENTS];
	ftp_clients_stats_t stats;
	bool inited;
} ftp_clients_t;

static const char *ftp_cmd_resp_421 = "421 No more connections allowed\r\n";
static char ftp_user_name[FTP_USER_NAME_LEN + 1] = FTP_USER_NAME_DEFAULT;
static char ftp_user_pass[FTP_USER_PASS_LEN + 1] = FTP_USER_PASS_DEFAULT;
static ftp_clients_t ftp_clients = { 0 };
FTP_STRUCT_MEM_SECTION(static char buffs[FTP_NBR_CLIENTS][FTP_CMD_BUFF_SIZE]) = {0};

void ftp_client_refresh(uint8_t index) {
	ftp_clients.client[index].idle_cnt = 0;
	ftp_clients.client[index].sent_idle_cnt = 0;
}

bool ftp_is_logged_in(uint8_t index) {
	return (ftp_clients.client[index].user == FTP_USER_USER_LOGGED_IN);
}

ftp_user_t ftp_get_user(uint8_t index) {
	return (ftp_clients.client[index].user);
}

void ftp_set_user(uint8_t index, ftp_user_t user) {
	ftp_clients.client[index].user = user;
}

bool ftp_is_user_name_ok(char *name, uint16_t len) {
	return (!strncmp(name, ftp_user_name, len));
}

bool ftp_is_pass_ok(char *pass, uint16_t len) {
	return (!strncmp(pass, ftp_user_pass, len));
}

char* ftp_get_path(uint8_t index) {
	return (ftp_clients.client[index].path);
}

void ftp_set_data_conn_mode(uint8_t index, dcm_type mode) {
	ftp_clients.client[index].data_conn_mode = mode;
}

dcm_type ftp_get_data_conn_mode(uint8_t index) {
	return (ftp_clients.client[index].data_conn_mode);
}

static void ftp_cmd_resp_send_cb(void *ctx) {
	ftp_client_t *client = (ftp_client_t*) ctx;
	if (client->client_pcb != NULL && client->len != 0) {
		tcp_write(client->client_pcb, client->buff, client->len, 0);
		DEBUG_PRINT(client->index, "CMD response sending...\r\n");
	}
}

ftp_result_t ftp_cmd_resp_send(uint8_t index, const char *fmt, ...) {
	ftp_client_t *client = &(ftp_clients.client[index]);
	if (xSemaphoreTake(client->buff_available_sem, 2000) == pdTRUE) {
		if (xSemaphoreTake(client->mutex, 2000) == pdTRUE) {
			va_list args;
			va_start(args, fmt);
			int full_len = vsnprintf(client->buff, FTP_CMD_BUFF_SIZE, fmt, args);
			va_end(args);

			ftp_result_t res = FTP_RES_OK;
			if (full_len <= 0) {
				DEBUG_PRINT(client->index, "Error on parsing CMD response\r\n");
				res = FTP_RES_ERROR;
			} else if (full_len >= FTP_CMD_BUFF_SIZE) {
				DEBUG_PRINT(client->index, "CMD response NOT written fully\r\n");
				res = FTP_RES_ERROR;
			} else {
				client->len = full_len;
				err_t err = tcpip_callbackmsg_trycallback(client->cb);
				if (err == ERR_OK) {
					DEBUG_PRINT(client->index, "CMD response waits to send...\r\n");
					res = FTP_RES_OK;
				} else {
					DEBUG_PRINT(client->index, "Error on sending CMD response: %d\r\n", err);
					res = FTP_RES_ERROR;
				}
			}
			xSemaphoreGive(client->mutex);
			return (res);
		} else {
			xSemaphoreGive(client->buff_available_sem);
			DEBUG_PRINT(client->index, "Timeout on CMD response mutex take\r\n");
			return (FTP_RES_TIMEOUT);
		}
	} else {
		DEBUG_PRINT(client->index, "Timeout on CMD response send\r\n");
		return (FTP_RES_TIMEOUT);
	}
}

static void ftp_client_clean(ftp_client_t *client) {
	client->idle_cnt = 0;
	client->client_pcb = NULL;
	client->len = 0;
	client->data_conn_mode = DCM_NOT_SET;
	xSemaphoreGive(client->buff_available_sem);
}

static void ftp_client_after_close(ftp_client_t *client) {
	FTP_DISCONNECTED_CALLBACK();
	FTP_LOG_PRINT("FTP client %d disconnected\r\n", client->index);
	ftp_pasv_listener_stop(client->index);
	if (ftp_clients.stats.clients_connected) {
		ftp_clients.stats.clients_connected--;
		ftp_clients.stats.clients_closed++;
	}
	ftp_client_clean(client);
}

static err_t ftp_client_close(ftp_client_t *client) {
	if (xSemaphoreTake(client->mutex, 1000) == pdTRUE) {
		if (tcp_close(client->client_pcb) == ERR_OK) {
			ftp_client_after_close(client);
			xSemaphoreGive(client->mutex);
			return (ERR_OK);
		} else {
			tcp_abort(client->client_pcb);
			xSemaphoreGive(client->mutex);
			return (ERR_ABRT);
		}
	} else {
		DEBUG_PRINT(client->index, "Timeout on CMD response mutex take, on clear\r\n");
		tcp_abort(client->client_pcb);
		xSemaphoreGive(client->mutex);
		return (ERR_ABRT);
	}
}

static void ftp_client_close_cb(void *ctx) {
	ftp_client_t *client = (ftp_client_t*) ctx;
	if (client->client_pcb) {
		ftp_client_close(client);
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
static void ftp_client_err(void *arg, err_t err) {
	UNUSED(err);
	ftp_client_t *client = (ftp_client_t*) arg;
	ftp_clients.stats.clients_errors++;
	ftp_client_after_close(client);
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
		return (ftp_client_close(client));
	}

	if (client->len) {
		client->sent_idle_cnt++;
		if (client->sent_idle_cnt >= FTP_TCP_SENT_MAX_IDLE_SEC) {
			ftp_clients.stats.clients_timeouts++;
			return (ftp_client_close(client));
		}
	} else {
		client->sent_idle_cnt = 0;
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
	client->idle_cnt = 0;
	client->sent_idle_cnt = 0;

	if (client->len <= len) {
		client->len = 0;
	} else {
		client->len -= len;
	}
	if (client->len == 0) {
		xSemaphoreGive(client->buff_available_sem);
	}
	DEBUG_PRINT(client->index, "CMD response sent\r\n");
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
		return (ftp_client_close(client));
	}

	client->idle_cnt = 0;
	ftp_cmd_msg_t msg = { .index = client->index, .p = p };
	err_t res = ftp_cmd_handle(&msg);
	tcp_recved(tpcb, p->tot_len);
	pbuf_free(p);
	return (res);
}

err_t ftp_client_start(struct tcp_pcb *newpcb) {
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
			strncpy(ftp_clients.client[i].path, "/", FTP_CWD_SIZE);
			ftp_clients.client[i].user = FTP_USER_NONE;

			tcp_arg(ftp_clients.client[i].client_pcb, &ftp_clients.client[i]);
			tcp_err(ftp_clients.client[i].client_pcb, ftp_client_err);
			tcp_poll(ftp_clients.client[i].client_pcb, ftp_client_poll, 2);
			tcp_sent(ftp_clients.client[i].client_pcb, ftp_client_sent);
			tcp_recv(ftp_clients.client[i].client_pcb, ftp_client_recv);

			ftp_clients.stats.clients_accepted++;
			ftp_clients.stats.clients_connected++;
			ftp_cmd_resp_send(i, "220 -> CMS FTP Server, FTP Version %s\r\n", FTP_VERSION);
			return (ERR_OK);
		}
	}

	ftp_clients.stats.clients_rejected++;
	tcp_write(newpcb, ftp_cmd_resp_421, strlen(ftp_cmd_resp_421), TCP_WRITE_FLAG_COPY);
	tcp_output(newpcb);
	tcp_abort(newpcb);
	return (ERR_ABRT);
}

void ftp_client_stop(uint8_t index) {
	if (ftp_clients.client[index].client_pcb) {
		ftp_client_close(&(ftp_clients.client[index]));
	}
}

err_t ftp_client_stop_ex(uint8_t index) {
	return (tcpip_callbackmsg_trycallback(ftp_clients.client[index].close_cb));
}

void ftp_clients_stop(void) {
	for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
		if (ftp_clients.client[i].client_pcb) {
			ftp_client_close(&(ftp_clients.client[i]));
		}
	}
}

void ftp_clients_init(void) {
	if (!ftp_clients.inited) {
		ftp_clients.inited = true;
		ftp_clients.stats.clients_max = FTP_NBR_CLIENTS;
		for (uint8_t i = 0; i < FTP_NBR_CLIENTS; ++i) {
			ftp_clients.client[i].index = i;
			ftp_clients.client[i].buff = buffs[i];
			ftp_clients.client[i].idle_cnt = 0;
			ftp_clients.client[i].len = 0;
			ftp_clients.client[i].data_conn_mode = DCM_NOT_SET;
			ftp_clients.client[i].buff_available_sem = xSemaphoreCreateBinary();
			assert_param(ftp_clients.client[i].buff_available_sem != NULL);
			xSemaphoreGive(ftp_clients.client[i].buff_available_sem);
			ftp_clients.client[i].mutex = xSemaphoreCreateMutex();
			assert_param(ftp_clients.client[i].mutex != NULL);
			xSemaphoreGive(ftp_clients.client[i].mutex);
			ftp_clients.client[i].cb = tcpip_callbackmsg_new(ftp_cmd_resp_send_cb, &(ftp_clients.client[i]));
			assert_param(ftp_clients.client[i].cb != NULL);
			ftp_clients.client[i].close_cb = tcpip_callbackmsg_new(ftp_client_close_cb, &(ftp_clients.client[i]));
			assert_param(ftp_clients.client[i].close_cb != NULL);
		}
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
