/*
 * ftp_cmd.c
 *
 *  Created on: Sep 19, 2025
 *      Author: jakub czekaj
 */

#include <string.h>
#include "ftp_cmd.h"
#include "ftp_config.h"
#include "stream_buffer.h"
#include "ftp_data.h"

#define FTP_PARAM_SIZE					_MAX_LFN + 8
#define FTP_CMD_SIZE					5
#define FTP_CMD_BUFFER_MESSAGE_SIZE		sizeof(ftp_cmd_msg_t)
#define FTP_CMD_BUFFER_SIZE				(FTP_CMD_BUFFER_MESSAGE_SIZE * 10)
#define FTP_CMD_TASK_SIZE				1024

static StreamBufferHandle_t ftp_cmd_buffer_handle = NULL;
static TaskHandle_t ftp_cmd_task_handle = NULL;

typedef struct {
	char *path;
	FILINFO finfo;
} ftp_cmd_temp_t;

typedef struct {
	ftp_cmd_msg_client_t *client;
	char *parameters;
	uint16_t parameters_len;
	ftp_cmd_temp_t temp;
} ftp_cmd_handler_data_t;

typedef struct {
	const char *cmd;
	ftp_result_t (*func)(const ftp_cmd_handler_data_t *const data);
} ftp_cmd_handlers_t;

typedef struct {
	char *command;
	uint16_t command_len;
	char *parameters;
	uint16_t parameters_len;
} ftp_cmd_t;

static void set_path_to_root(char *path) {
	strncpy(path, "/", FTP_CWD_SIZE);
}

static void path_up_a_level(char *path) {
	char *last_dash = strrchr(path, '/');
	if (last_dash != NULL) {
		set_path_to_root(path);
	} else if (last_dash == path) {
		set_path_to_root(path);
	} else {
		memset(last_dash, 0, strlen(last_dash));
	}
}

static uint8_t path_build(char *path, char *parameters, uint16_t parameters_len) {
	if (parameters_len == 0) {
		set_path_to_root(path);
		return (1);
	} else if (parameters[0] == '/') {
		if (parameters_len == 1) {
			set_path_to_root(path);
			return (1);
		} else if (parameters_len < FTP_CWD_SIZE) {
			strncpy(path, parameters, FTP_CWD_SIZE);
			uint16_t strl = strlen(path) - 1;
			if (path[strl] == '/' && strl > 1) {
				path[strl] = 0;
			}
			return (1);
		} else {
			return (0);
		}
	} else if (parameters[0] == '.' && parameters[1] == '.') {
		path_up_a_level(path);
		return (1);
	} else {
		if (strlen(path) + 1 + parameters_len < FTP_CWD_SIZE) {
			if (path[strlen(path) - 1] != '/') {
				strncat(path, "/", FTP_CWD_SIZE);
			}
			strncat(path, parameters, FTP_CWD_SIZE);
			uint16_t strl = strlen(path) - 1;
			if (path[strl] == '/' && strl > 1) {
				path[strl] = 0;
			}
			return (1);
		} else {
			return (0);
		}
	}
}

static ftp_result_t ftp_cmd_noop(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	return (ftp_send(data->client, "200 zzz...\r\n"));
}

static ftp_result_t ftp_cmd_quit(const ftp_cmd_handler_data_t *const data) {
	return (ftp_send(data->client, "221 Goodbye\r\n"));
}

// print working directory
static ftp_result_t ftp_cmd_pwd(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	} else {
		return (ftp_send(data->client, "257 \"%s\" is your current directory\r\n", ftp_get_path(data->client)));
	}
}

// change working directory
static ftp_result_t ftp_cmd_cwd(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	if (data->parameters_len == 0) {
		return (ftp_send(data->client, "501 No directory name\r\n"));
	}
	data->temp.path = ftp_get_path(data->client);
	if (!path_build(data->temp.path, data->parameters)) {
		return (ftp_send(data->client, "500 Command line too long\r\n"));
	}
	if (strcmp(data->temp.path, "/") != 0 && FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK) {
		return (ftp_send(data->client, "550 Failed to change directory to %s\r\n", data->temp.path));
	}

	return (ftp_send(data->client, "250 Directory successfully changed.\r\n"));
}

// Change the remote machine working directory to the parent of the current remote machine working directory.
static ftp_result_t ftp_cmd_cdup(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	set_path_to_root(ftp_get_path(data->client));
	return (ftp_send(data->client, "250 Directory successfully changed to root.\r\n"));
}

// change mode
static ftp_result_t ftp_cmd_mode(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "S")) {
		return (ftp_send(data->client, "200 S OK\r\n"));
	} else {
		return (ftp_send(data->client, "504 Only S(tream) is supported\r\n"));
	}
}

static ftp_result_t ftp_cmd_stru(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "F")) {
		return (ftp_send(data->client, "200 F OK\r\n"));
	} else {
		return (ftp_send(data->client, "504 Only F(ile) is supported\r\n"));
	}
}

static ftp_result_t ftp_cmd_type(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "A")) {
		return (ftp_send(data->client, "200 TYPE is now ASCII\r\n"));
	} else if (!strcmp(data->parameters, "I")) {
		return (ftp_send(data->client, "200 TYPE is now 8-bit binary\r\n"));
	} else {
		return (ftp_send(data->client, "504 Unknown TYPE\r\n"));
	}
}

static ftp_result_t ftp_cmd_pasv(const ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->client)) {
		return (FTP_RES_OK);
	}
#if FTP_USE_PASSIVE_MODE == 1
	// set data port
	ftp->data_port = FTP_DATA_PORT + ftp->data_port_incremented + (ftp->ftp_con_num * PORT_INCREMENT_OFFSET);

	// open connection ok?
	if (pasv_con_open(ftp) == FTP_RES_OK) {
		// close data connection, just to be sure
		if (data_con_close(ftp) != FTP_RES_OK) {
			return (pasv_con_close(ftp));
		}
		// feedback
		DEBUG_PRINT(data->client->index, "Data port set to %u\r\n", ftp->data_port);
		// set state
		ftp->data_conn_mode = DCM_PASSIVE;
		// reply that we are entering passive mode
		return (ftp_send(data->client, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).\r\n", ftp->ipserver.addr & 0xFF, (ftp->ipserver.addr >> 8) & 0xFF,
				(ftp->ipserver.addr >> 16) & 0xFF, (ftp->ipserver.addr >> 24) & 0xFF, ftp->data_port >> 8, ftp->data_port & 255));
	} else {
		ftp_set_connection_mode(data->client, DCM_NOT_SET);
		ftp_send(data->client, "425 Can't set connection management to passive\r\n");
		return (FTP_RES_ERROR);
	}
#else
	ftp->dataConnMode = DCM_NOT_SET;
	return (ftp_send(ftp, "421 Passive mode not available\r\n"));
#endif
}

static ftp_cmd_handlers_t ftpd_commands[] = { //
		{ "NOOP", ftp_cmd_noop }, //
		{ "QUIT", ftp_cmd_quit }, //
		{ "PWD", ftp_cmd_pwd }, // print working directory
		{ "CWD", ftp_cmd_cwd }, // change working directory
		{ "CDUP", ftp_cmd_cdup }, // Change working directory to root
		{ "MODE", ftp_cmd_mode }, // change mode
		{ "STRU", ftp_cmd_stru }, //
		{ "TYPE", ftp_cmd_type }, //
		{ "PASV", ftp_cmd_pasv }, //
//		{ "PORT", ftp_cmd_port }, //
//		{ "NLST", ftp_cmd_list }, //
//		{ "LIST", ftp_cmd_list }, //
//		{ "MLSD", ftp_cmd_mlsd }, //
//		{ "DELE", ftp_cmd_dele }, //
//		{ "RETR", ftp_cmd_retr }, //
//		{ "STOR", ftp_cmd_stor }, //
//		{ "MKD", ftp_cmd_mkd }, //
//		{ "RMD", ftp_cmd_rmd }, //
//		{ "RNFR", ftp_cmd_rnfr }, //
//		{ "RNTO", ftp_cmd_rnto }, //
//		{ "FEAT", ftp_cmd_feat }, //
//		{ "MDTM", ftp_cmd_mdtm }, //
//		{ "SIZE", ftp_cmd_size }, //
//		{ "SITE", ftp_cmd_site }, //
//		{ "STAT", ftp_cmd_stat }, //
//		{ "SYST", ftp_cmd_syst }, //
//		{ "AUTH", ftp_cmd_auth }, //
//		{ "USER", ftp_cmd_user }, //
//		{ "PASS", ftp_cmd_pass }, //
		{ NULL, NULL } //
		};

static ftp_result_t ftp_process_command(const ftp_cmd_msg_client_t *const client, const ftp_cmd_t *const ftp_cmd) {
	ftp_cmd_handlers_t *handler = ftpd_commands;
	uint16_t cmd_len = 0;
	while (handler->cmd != NULL && handler->func != NULL) {
		cmd_len = strlen(handler->cmd);
		if (cmd_len == ftp_cmd->command_len) {
			if (!strncmp(handler->cmd, ftp_cmd->command, cmd_len)) {
				ftp_cmd_handler_data_t data = { .client = client, .parameters = ftp_cmd->parameters, .parameters_len = ftp_cmd->parameters_len };
				return (handler->func(&data));
			}
		}
		handler++;
	}
	return (ftp_send(client, "500 Unknown command\r\n"));
}

static ftp_result_t ftp_parse_command(struct pbuf *p, ftp_cmd_t *const ftp_cmd) {
	uint32_t ret = 0;
	char *pbuf = (char*) p->payload;
	uint16_t buflen = p->len;
	uint32_t i = 0;
	memset(ftp_cmd, 0, sizeof(ftp_cmd_t));

	if (buflen == 0) {
		return (FTP_RES_ERROR);
	} else {
		do {
			if (isalpha((uint8_t ) pbuf[i])) {
				if (ftp_cmd->command == NULL) {
					ftp_cmd->command = &(pbuf[i]);
				}
				i++;
			} else {
				break;
			}
		} while (i < buflen && i < (FTP_CMD_SIZE - 1));
		if (ftp_cmd->command == NULL) {
			return (FTP_RES_ERROR);
		} else {
			ftp_cmd->command_len = i;
			if (pbuf[i] == ' ') {
				while (pbuf[i] == ' ') {
					i++;
				}
				while (pbuf[i + ret] != '\n' && pbuf[i + ret] != '\r' && (i + ret) < buflen) {
					ret++;
				}
				if (ret + 1 >= FTP_PARAM_SIZE) {
					return (FTP_RES_ERROR);
				} else {
					ftp_cmd->parameters = pbuf + i;
					ftp_cmd->parameters_len = (uint16_t) ret;
					return (FTP_RES_OK);
				}
			} else {
				return (FTP_RES_ERROR);
			}
		}
	}
}

__NO_RETURN static void ftp_cmd_task(void *pvParameters) {
	UNUSED(pvParameters);
	ftp_cmd_msg_t msg = { 0 };
	ftp_cmd_t ftp_cmd = { 0 };

	for (;;) {
		if (xStreamBufferReceive(ftp_cmd_buffer_handle, &msg, FTP_CMD_BUFFER_MESSAGE_SIZE, portMAX_DELAY) != FTP_CMD_BUFFER_MESSAGE_SIZE) {
			continue;
		}
		if (ftp_parse_command(msg.p, &ftp_cmd) == FTP_RES_OK) {
			DEBUG_PRINT(msg.client.index, "Incomming: %.*s %.*s\r\n", ftp_cmd.command_len, ftp_cmd.command, ftp_cmd.parameters_len, ftp_cmd.parameters);
			if (ftp_process_command(&msg.client, &ftp_cmd) != FTP_RES_OK) {
				DEBUG_PRINT(msg.client.index, "CMD process: FAILED\r\n");
			} else {
				DEBUG_PRINT(msg.client.index, "CMD process: SUCCESS\r\n");
			}
		} else {
			DEBUG_PRINT(msg.client.index, "Wrong command: %.*s\r\n", msg.p->len, msg.p->payload);
		}
		pbuf_free(msg.p);
	}
}

err_t ftp_cmd_handle(const ftp_cmd_msg_t *const msg) {
	pbuf_ref(msg->p);
	if (xStreamBufferSend(ftp_cmd_buffer_handle, msg, FTP_CMD_BUFFER_MESSAGE_SIZE, 0) == FTP_CMD_BUFFER_MESSAGE_SIZE) {
		DEBUG_PRINT(index, "ftp cmd buff send OK\r\n");
	} else {
		DEBUG_PRINT(index, "ftp cmd buff send ERROR\r\n");
	}
	return (ERR_OK);
}

void ftp_cmd_init(void) {
	ftp_cmd_buffer_handle = xStreamBufferCreate(FTP_CMD_BUFFER_SIZE, FTP_CMD_BUFFER_MESSAGE_SIZE);
	assert_param(ftp_cmd_buffer_handle != NULL);
	assert_param(xTaskCreate(ftp_cmd_task, "FTP CMD", FTP_CMD_TASK_SIZE, NULL, osPriorityNormal, &ftp_cmd_task_handle) == pdPASS);
}

