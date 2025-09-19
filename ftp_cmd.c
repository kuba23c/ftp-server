/*
 * ftp_cmd.c
 *
 *  Created on: Sep 19, 2025
 *      Author: jakub czekaj
 */

#include "ftp_cmd.h"
#include "ftp_config.h"

#define DEBUG_PRINT(i, f, ...)	FTP_LOG_PRINT("[%d] "f, i, ##__VA_ARGS__)
#define FTP_PARAM_SIZE				_MAX_LFN + 8
#define FTP_CMD_SIZE				5

typedef enum {
	FTP_RES_OK,
	FTP_RES_TIMEOUT,
	FTP_RES_ERROR
} ftp_result_t;

typedef struct {
	const char *cmd;
	ftp_result_t (*func)(ftp_data_t *ftp);
} ftp_cmd_handlers_t;

typedef struct {
	char command[FTP_CMD_SIZE];
	char parameters[FTP_PARAM_SIZE];
} ftp_cmd_t;

static ftp_cmd_t ftp_cmd = { 0 };

static int ftp_parse_command_check(struct pbuf *p) {
	int ret = 0;
	char *pbuf = (char*) p->payload;
	uint16_t buflen = p->len;

	if (buflen != 0) {
		int8_t i = 0;
		do {
			if (!isalpha((uint8_t ) pbuf[i])) {
				break;
			}
			ftp_cmd.command[i] = pbuf[i];
			i++;
		} while (i < buflen && i < (FTP_CMD_SIZE - 1));
		if (pbuf[i] == ' ') {
			while (pbuf[i] == ' ') {
				i++;
			}
			while (pbuf[i + ret] != '\n' && pbuf[i + ret] != '\r' && (i + ret) < buflen) {
				ret++;
			}
			if (ret + 1 >= FTP_PARAM_SIZE) {
				ret = -1;
			} else {
				strncpy(ftp_cmd.parameters, pbuf + i, ret);
			}
		}
	}
	return (ret);
}

// Parse the last command
// return: -1 syntax error
//          0 command without parameters
//          >0 length of parameters
static ftp_result_t ftp_parse_command(struct pbuf *p) {
	memset(ftp_cmd.command, 0, FTP_CMD_SIZE);
	memset(ftp_cmd.parameters, 0, FTP_PARAM_SIZE);

	int ret = ftp_parse_command_check(p);

	netbuf_delete(ftp->inbuf);
	if (ret < 0) {
		return (FTP_RES_ERROR);
	} else {
		return (FTP_RES_OK);
	}
}

err_t ftp_cmd_handle(uint8_t index, struct tcp_pcb *tpcb, struct pbuf *p) {
//	pbuf_ref(p)
	if (ftp_parse_command() != FTP_RES_OK) {
		DEBUG_PRINT(index, "Wrong command: %.*s\r\n", p->len, p->payload);
		return (ERR_OK);
	}
	DEBUG_PRINT(index, "Incomming: %s %s\r\n", ftp_cmd.command, ftp_cmd.parameters);
}

