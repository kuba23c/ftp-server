/*
 FTP Server for STM32-E407 and ChibiOS
 Copyright (C) 2015 Jean-Michel Gallego

 See readme.txt for information

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

#include "ftp_server.h"
#include "lwip.h"
#include "FreeRTOS.h"
#include "ftp_config.h"

// stdlib
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>

// RTOS include
#include "event_groups.h"
// lwip include
#include "api.h"

#define FTP_VERSION				"2020-08-20"
#define FTP_PARAM_SIZE			_MAX_LFN + 8
#define FTP_CWD_SIZE			_MAX_LFN + 8
#define FTP_CMD_SIZE			5
#define FTP_DATE_STRING_SIZE	64
#define PORT_INCREMENT_OFFSET	25 // used for a bugfix which works around ports which are already in use (from a previous connection)
#define DEBUG_PRINT(ftp, f, ...)	FTP_LOG_PRINT("[%d] "f, ftp->ftp_con_num, ##__VA_ARGS__)
#define FTP_USER_NAME_OK(name)		(!strcmp(name, ftp_user_name))
#define FTP_USER_PASS_OK(pass)		(!strcmp(pass, ftp_user_pass))
#define FTP_IS_LOGGED_IN(p_ftp)		(p_ftp->user == FTP_USER_USER_LOGGED_IN)
#define FTP_BUF_SIZE_MIN 			1024
#define FTP_BUF_SIZE 				(FTP_BUF_SIZE_MIN * FTP_BUF_SIZE_MULT)

typedef enum {
	FTP_RES_OK,
	FTP_RES_TIMEOUT,
	FTP_RES_ERROR
} ftp_result_t;

// Data Connection mode enumeration typedef
typedef enum {
	DCM_NOT_SET,
	DCM_PASSIVE,
	DCM_ACTIVE
} dcm_type;

// ftp log in enumeration typedef
typedef enum {
	FTP_USER_NONE,
	FTP_USER_USER_NO_PASS,
	FTP_USER_USER_LOGGED_IN
} ftp_user_t;

/**
 * Structure that contains all variables used in FTP connection.
 * This is not nicely done since code is ported from C++ to C. The
 * C++ private object variables are listed inside this structure.
 */
typedef struct {
	// sockets
	struct netconn *listdataconn;
	struct netconn *dataconn;
	struct netconn *ctrlconn;
	struct netbuf *inbuf;

	// ip addresses
	ip4_addr_t ipclient;
	ip4_addr_t ipserver;

	// port
	uint16_t data_port;
	uint8_t data_port_incremented;

	// file variables, not created on stack but static on boot
	// to avoid overflow and ensure alignment in memory
	FIL file;
	FILINFO finfo;
	// buffer for command sent by client
	char command[FTP_CMD_SIZE];

	// buffer for parameters sent by client
	char parameters[FTP_PARAM_SIZE];

	// buffer for origin path for Rename command
	char path_rename[FTP_CWD_SIZE];

	// buffer for path that is currently used
	char path[FTP_CWD_SIZE];

	// buffer for writing/reading to/from memory
	ALIGN_32BYTES(char ftp_buff[FTP_BUF_SIZE]);

	// date string buffer
	char date_str[FTP_DATE_STRING_SIZE];

	// connection mode (not set, active or passive)
	uint8_t ftp_con_num;

	// state which tells which user is logged in
	ftp_user_t user;

	// data connection mode state
	dcm_type data_conn_mode;
} ftp_data_t;

// structure for ftp commands
typedef struct {
	const char *cmd;
	ftp_result_t (*func)(ftp_data_t *ftp);
} ftp_cmd_t;

// define a structure of parameters for a ftp thread
typedef struct {
	uint8_t number;
	struct netconn *ftp_connection;
	TaskHandle_t task_handle;
#if FTP_TASK_STATIC == 1
	StackType_t task_stack[FTP_TASK_STACK_SIZE];
	StaticTask_t task_static;
#endif
	ftp_data_t ftp_data;
	bool busy;
	bool stop;
} server_stru_t;

typedef struct {
	TaskHandle_t server_task_handle;
	ftp_status_t status;
	ftp_stats_t stats;
	uint16_t port;
	uint32_t errors;
	bool inited;
} ftp_t;

static char ftp_user_name[FTP_USER_NAME_LEN + 1] = FTP_USER_NAME_DEFAULT;
static char ftp_user_pass[FTP_USER_PASS_LEN + 1] = FTP_USER_PASS_DEFAULT;
static ftp_t FTP = { 0 };
static const char *no_conn_allowed = "421 No more connections allowed\r\n";
FTP_STRUCT_MEM_SECTION(static server_stru_t ftp_links[FTP_NBR_CLIENTS]) = {0};
// =========================================================
//
//              Send a response to the client
//
// =========================================================

static void ftp_set_error(ftp_error_t error) {
	FTP.status = FTP_ERROR_STOPPING;
	FTP.errors |= ((uint32_t) 1) << error;
}

#undef netconn_write

static ftp_result_t wait_for_netconn_write_finish(struct netconn *conn, size_t *bytes_written, size_t size) {
	uint32_t timeout_cnt = 0;
	ftp_result_t res = FTP_RES_OK;
	while (*bytes_written != size || conn->state != NETCONN_NONE) {
		vTaskDelay(1);
		timeout_cnt++;
		if (timeout_cnt >= FTP_SERVER_WRITE_TIMEOUT_MS) {
			res = FTP_RES_TIMEOUT;
			FTP_LOG_PRINT("NETCONN WRITE TIMEOUT!!!\r\n");
			break;
		}
	}
	return (res);
}

ftp_result_t netconn_write(struct netconn *conn, const void *dataptr, size_t size) {
	size_t bytes_written = 0;
	ftp_result_t res = FTP_RES_OK;
	err_t err = netconn_write_partly(conn, dataptr, size, NETCONN_COPY, &bytes_written);
	if (err == ERR_INPROGRESS) {
		res = wait_for_netconn_write_finish(conn, &bytes_written, size);
	} else if (err != ERR_OK) {
		FTP_LOG_PRINT("client NETCONN write error\r\n");
		ftp_set_error(FTP_ERROR_CLIENT_NETCONN_WRITE);
		res = FTP_RES_ERROR;
	}
	return (res);
}

static ftp_result_t ftp_send(ftp_data_t *ftp, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(ftp->ftp_buff, FTP_BUF_SIZE, fmt, args);
	va_end(args);

	DEBUG_PRINT(ftp, "%s", ftp->ftp_buff);
	return (netconn_write(ftp->ctrlconn, ftp->ftp_buff, strlen(ftp->ftp_buff)));
}

// Create string YYYYMMDDHHMMSS from date and time
//
// parameters:
//    date, time
//
// return:
//    pointer to string

static char* data_time_to_str(char *str, uint16_t date, uint16_t time) {
	snprintf(str, 25, "%04d%02d%02d%02d%02d%02d", ((date & 0xFE00) >> 9) + 1980, (date & 0x01E0) >> 5, date & 0x001F, (time & 0xF800) >> 11,
			(time & 0x07E0) >> 5, (time & 0x001F) << 1);
	return (str);
}

// Calculate date and time from first parameter sent by MDTM command (YYYYMMDDHHMMSS)
//
// parameters:
//   pdate, ptime: pointer of variables where to store data
//
// return:
//    length of (time parameter + space) if date/time are ok
//    0 if parameter is not YYYYMMDDHHMMSS

static uint8_t date_time_get(char *parameters, uint16_t *pdate, uint16_t *ptime) {
	// Date/time are expressed as a 14 digits long string
	//   terminated by a space and followed by name of file
	if (strlen(parameters) < 15 || parameters[14] != ' ')
		return (0);
	for (uint8_t i = 0; i < 14; i++)
		if (!isdigit((uint8_t ) parameters[i]))
			return (0);

	parameters[14] = 0;
	*ptime = atoi(parameters + 12) >> 1;   // seconds
	parameters[12] = 0;
	*ptime |= atoi(parameters + 10) << 5;  // minutes
	parameters[10] = 0;
	*ptime |= atoi(parameters + 8) << 11;  // hours
	parameters[8] = 0;
	*pdate = atoi(parameters + 6);         // days
	parameters[6] = 0;
	*pdate |= atoi(parameters + 4) << 5;   // months
	parameters[4] = 0;
	*pdate |= (atoi(parameters) - 1980) << 9;       // years

	return (15);
}

// =========================================================
//
//             Get a command from the client
//
// =========================================================

static ftp_result_t ftp_read_command(ftp_data_t *ftp, bool *stop) {
	// loop and check for packet every second
	ftp_result_t res = FTP_RES_OK;
	err_t err = ERR_OK;
	for (uint32_t i = 0; i < FTP_SERVER_INACTIVE_CNT; i++) {
		if (*stop == true || FTP.stats == FTP_ERROR || FTP.stats == FTP_ERROR_STOPPING) {
			res = FTP_RES_ERROR;
			DEBUG_PRINT(ftp, "NETCONN CLIENT STOP!\r\n");
			break;
		}
		err = netconn_recv(ftp->ctrlconn, &ftp->inbuf);
		if (err == ERR_TIMEOUT) {
			if (!FTP_ETH_IS_LINK_UP()) {
				res = FTP_RES_ERROR;
				DEBUG_PRINT(ftp, "ETH link down!\r\n");
				break;
			}
			continue;
		} else if (err == ERR_OK) {
			break;
		} else {
			res = FTP_RES_ERROR;
			DEBUG_PRINT(ftp, "NETCONN RECV ERROR: %d\r\n", err);
			break;
		}
	}
	if (res == FTP_RES_OK && err == ERR_TIMEOUT) {
		res = FTP_RES_TIMEOUT;
		DEBUG_PRINT(ftp, "NETCONN RECV TIMEOUT\r\n");
	}
	return (res);
}

static int ftp_parse_command_check(ftp_data_t *ftp) {
	int ret = 0;
	char *pbuf;
	uint16_t buflen;

	// get data from recieved packet
	netbuf_data(ftp->inbuf, (void**) &pbuf, &buflen);
	if (buflen != 0) {
		int8_t i = 0;
		do {
			if (!isalpha((uint8_t ) pbuf[i])) {
				break;
			}
			ftp->command[i] = pbuf[i];
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
				strncpy(ftp->parameters, pbuf + i, ret);
			}
		}
	}
	return (ret);
}

// =========================================================
//
//             Parse the last command
//
// =========================================================
// return: -1 syntax error
//          0 command without parameters
//          >0 length of parameters

static ftp_result_t ftp_parse_command(ftp_data_t *ftp) {
	memset(ftp->command, 0, FTP_CMD_SIZE);
	memset(ftp->parameters, 0, FTP_PARAM_SIZE);

	int ret = ftp_parse_command_check(ftp);

	DEBUG_PRINT(ftp, "Incomming: %s %s\r\n", ftp->command, ftp->parameters);
	netbuf_delete(ftp->inbuf);
	if (ret < 0) {
		return (FTP_RES_ERROR);
	} else {
		return (FTP_RES_OK);
	}
}

// =========================================================
//
//               Functions for data connection
//
// =========================================================

static ftp_result_t pasv_con_open(ftp_data_t *ftp) {
	if (ftp->listdataconn != NULL) {
		return (FTP_RES_OK);
	}
	ftp->listdataconn = netconn_new(NETCONN_TCP);
	if (ftp->listdataconn == NULL) {
		DEBUG_PRINT(ftp, "Error in opening listening con, creation failed\r\n");
		ftp_set_error(FTP_ERROR_LISTEN_DATA_NETCONN_NEW);
		return (FTP_RES_ERROR);
	}
	// Bind listdataconn to port (FTP_DATA_PORT + num) with default IP address
	int8_t err = netconn_bind(ftp->listdataconn, IP_ADDR_ANY, ftp->data_port);
	if (err != ERR_OK) {
		DEBUG_PRINT(ftp, "Error in opening listening con, bind failed %d\r\n", err);
		ftp_set_error(FTP_ERROR_LISTEN_DATA_NETCONN_BIND);
		return (FTP_RES_ERROR);
	}
	netconn_set_recvtimeout(ftp->listdataconn, FTP_PSV_LISTEN_TIMEOUT_MS);
	err = netconn_listen(ftp->listdataconn);
	if (err != ERR_OK) {
		DEBUG_PRINT(ftp, "Error in opening listening con, listen failed %d\r\n", err);
		ftp_set_error(FTP_ERROR_LISTEN_DATA_NETCONN_LISTEN);
		return (FTP_RES_ERROR);
	}
	return (FTP_RES_OK);
}

static ftp_result_t pasv_con_close(ftp_data_t *ftp) {
	ftp_result_t res = FTP_RES_OK;
	ftp->data_conn_mode = DCM_NOT_SET;
	if (ftp->listdataconn == NULL) {
		return (res);
	}
	if (netconn_close(ftp->listdataconn) != ERR_OK) {
		FTP_LOG_PRINT("listen data NETCONN close error\r\n");
		ftp_set_error(FTP_ERROR_LISTEN_DATA_NETCONN_CLOSE);
		res = FTP_RES_ERROR;
	}
	if (netconn_delete(ftp->listdataconn) != ERR_OK) {
		FTP_LOG_PRINT("listen data NETCONN delete error\r\n");
		ftp_set_error(FTP_ERROR_LISTEN_DATA_NETCONN_DELETE);
		res = FTP_RES_ERROR;
	}
	ftp->listdataconn = NULL;
	return (res);
}

static ftp_result_t data_con_open(ftp_data_t *ftp) {
	if (ftp->data_conn_mode == DCM_NOT_SET) {
		DEBUG_PRINT(ftp, "No connecting mode defined\r\n");
		return (FTP_RES_ERROR);
	}
	DEBUG_PRINT(ftp, "Data conn in %s mode\r\n", (ftp->data_conn_mode == DCM_PASSIVE ? "passive" : "active"));
	if (ftp->data_conn_mode == DCM_PASSIVE) {
		if (ftp->listdataconn == NULL) {
			return (FTP_RES_ERROR);
		}
		netconn_set_recvtimeout(ftp->listdataconn, FTP_PSV_ACCEPT_TIMEOUT_MS);
		if (netconn_accept(ftp->listdataconn, &ftp->dataconn) != ERR_OK) {
			DEBUG_PRINT(ftp, "Error in data conn: netconn_accept\r\n");
			return (FTP_RES_ERROR);
		}
		netconn_set_recvtimeout(ftp->dataconn, FTP_SERVER_READ_TIMEOUT_MS);
		netconn_set_sendtimeout(ftp->dataconn, FTP_SERVER_WRITE_TIMEOUT_MS);
	} else {
		ftp->dataconn = netconn_new(NETCONN_TCP);
		if (ftp->dataconn == NULL) {
			DEBUG_PRINT(ftp, "Error in data conn: netconn_new\r\n");
			ftp_set_error(FTP_ERROR_DATA_NETCONN_NEW);
			return (FTP_RES_ERROR);
		}
		if (netconn_bind(ftp->dataconn, IP_ADDR_ANY, 0) != ERR_OK) {
			DEBUG_PRINT(ftp, "Error in data conn: netconn_bind\r\n");
			ftp_set_error(FTP_ERROR_DATA_NETCONN_BIND);
			if (netconn_delete(ftp->dataconn) != ERR_OK) {
				ftp_set_error(FTP_ERROR_DATA_NETCONN_DELETE);
			}
			ftp->dataconn = NULL;
			return (FTP_RES_ERROR);
		}
		netconn_set_recvtimeout(ftp->dataconn, FTP_SERVER_READ_TIMEOUT_MS);
		netconn_set_sendtimeout(ftp->dataconn, FTP_SERVER_WRITE_TIMEOUT_MS);
		if (netconn_connect(ftp->dataconn, &ftp->ipclient, ftp->data_port) != ERR_OK) {
			DEBUG_PRINT(ftp, "Error in data conn: netconn_connect\r\n");
			if (netconn_delete(ftp->dataconn) != ERR_OK) {
				ftp_set_error(FTP_ERROR_DATA_NETCONN_DELETE);
			}
			ftp->dataconn = NULL;
			return (FTP_RES_ERROR);
		}
	}
	return (FTP_RES_OK);
}

static ftp_result_t data_con_close(ftp_data_t *ftp) {
	ftp_result_t res = FTP_RES_OK;

	ftp->data_conn_mode = DCM_NOT_SET;
	if (ftp->dataconn == NULL) {
		return (res);
	}
	if (netconn_close(ftp->dataconn) != ERR_OK) {
		FTP_LOG_PRINT("data NETCONN close error\r\n");
		ftp_set_error(FTP_ERROR_DATA_NETCONN_CLOSE);
		res = FTP_RES_ERROR;
	}
	if (netconn_delete(ftp->dataconn) != ERR_OK) {
		FTP_LOG_PRINT("data NETCONN delete error\r\n");
		ftp_set_error(FTP_ERROR_DATA_NETCONN_DELETE);
		res = FTP_RES_ERROR;
	}
	ftp->dataconn = NULL;
	return (res);
}

// =========================================================
//
//                  Functions on files
//
// =========================================================

static void path_up_a_level(char *path) {
	// is there a dash in the string?
	if (strchr(path, '/')) {
		// get position
		uint32_t pos = strlen(path) - 1;

		// go up a folder
		while (path[pos] != '/') {
			// clear character
			path[pos] = 0;

			// update position
			pos = strlen(path) - 1;
		}

		// remove the dash on which the wile loop exits, but only
		// when we are not root
		if (strlen(path) > 1)
			path[pos] = 0;
	}
}

// Make complete path/name from cwdName and parameters
//
// 3 possible cases:
//   parameters can be absolute path, relative path or only the name
//
// parameters:
//   fullName : where to store the path/name
//
// return:
//   true, if done

static uint8_t path_build(char *current_path, char *ftp_param) {
	// Should we go to the root directory or is the parameter buffer empty?
	if (!strcmp(ftp_param, "/") || strlen(ftp_param) == 0) {
		// go to root directory
		strncpy(current_path, "/", FTP_CWD_SIZE);
	}
	// should we go up a directory?
	else if (strcmp(ftp_param, "..") == 0) {
		// remove characters until '/' is found
		path_up_a_level(current_path);
	}
	// The incoming parameter doesn't contain a slash? this means that
	// the parameter is only the folder name and it should be appended
	else if (ftp_param[0] != '/') {
		// should we concatinate '/'?
		if (current_path[strlen(current_path) - 1] != '/')
			strncat(current_path, "/", FTP_CWD_SIZE);

		// concatinate parameter to string
		strncat(current_path, ftp_param, FTP_CWD_SIZE);
	}
	// The incoming parameter starts with a slash. This means that
	// the parameter is the whole path.
	else {
		strncpy(current_path, ftp_param, FTP_CWD_SIZE);
	}

	// If the string is longer than 1 character and ends with '/', remove it
	uint16_t strl = strlen(current_path) - 1;
	if (current_path[strl] == '/' && strl > 1)
		current_path[strl] = 0;

	// does the string fit? success
	if (strlen(current_path) < FTP_CWD_SIZE)
		return (1);

	// failed
	return (0);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//			FTP commands
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// print working directory
static ftp_result_t ftp_cmd_pwd(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	} else {
		return (ftp_send(ftp, "257 \"%s\" is your current directory\r\n", ftp->path));
	}
}

// change working directory
static ftp_result_t ftp_cmd_cwd(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No directory name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (strcmp(ftp->path, "/") != 0 && FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK) {
		return (ftp_send(ftp, "550 Failed to change directory to %s\r\n", ftp->path));
	}

	return (ftp_send(ftp, "250 Directory successfully changed.\r\n"));
}

// Change the remote machine working directory to the parent of the current remote machine working directory.
static ftp_result_t ftp_cmd_cdup(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	strncpy(ftp->path, "/", FTP_CWD_SIZE);
	return (ftp_send(ftp, "250 Directory successfully changed to root.\r\n"));
}

static ftp_result_t ftp_cmd_mode(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(ftp->parameters, "S")) {
		return (ftp_send(ftp, "200 S Ok\r\n"));
	} else {
		return (ftp_send(ftp, "504 Only S(tream) is suported\r\n"));
	}
}

static ftp_result_t ftp_cmd_stru(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(ftp->parameters, "F")) {
		return (ftp_send(ftp, "200 F Ok\r\n"));
	} else {
		return (ftp_send(ftp, "504 Only F(ile) is suported\r\n"));
	}
}

static ftp_result_t ftp_cmd_type(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(ftp->parameters, "A")) {
		return (ftp_send(ftp, "200 TYPE is now ASCII\r\n"));
	} else if (!strcmp(ftp->parameters, "I")) {
		return (ftp_send(ftp, "200 TYPE is now 8-bit binary\r\n"));
	} else {
		return (ftp_send(ftp, "504 Unknow TYPE\r\n"));
	}
}

static ftp_result_t ftp_cmd_pasv(ftp_data_t *ftp) {
	// are we not yet logged in?
	if (!FTP_IS_LOGGED_IN(ftp)) {
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
		DEBUG_PRINT(ftp, "Data port set to %u\r\n", ftp->data_port);
		// set state
		ftp->data_conn_mode = DCM_PASSIVE;
		// reply that we are entering passive mode
		return (ftp_send(ftp, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).\r\n", ftp->ipserver.addr & 0xFF, (ftp->ipserver.addr >> 8) & 0xFF,
				(ftp->ipserver.addr >> 16) & 0xFF, (ftp->ipserver.addr >> 24) & 0xFF, ftp->data_port >> 8, ftp->data_port & 255));
	} else {
		// reset data conn mode
		ftp->data_conn_mode = DCM_NOT_SET;
		// send error
		ftp_send(ftp, "425 Can't set connection management to passive\r\n");
		return (FTP_RES_ERROR);
	}
#else
	// reset data conn mode
	ftp->dataConnMode = DCM_NOT_SET;
	// send error
	return (ftp_send(ftp, "421 Passive mode not available\r\n"));
#endif
}

static ftp_result_t ftp_cmd_port(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	uint8_t ip[4];
	uint8_t i;

	if (data_con_close(ftp) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}

	if (strlen(ftp->parameters) == 0) {
		ftp->data_conn_mode = DCM_NOT_SET;
		return (ftp_send(ftp, "501 no parameters given\r\n"));
	}

	// Start building IP
	char *p = ftp->parameters - 1;
	for (i = 0; i < 4 && p != NULL; i++) {
		if (p == NULL) {
			break;
		}
		ip[i] = atoi(++p);
		p = strchr(p, ',');
	}

	if (p != NULL) {
		if (i == 4) {
			ftp->data_port = 256 * atoi(++p);
		}
		p = strchr(p, ',');
		if (p != NULL) {
			ftp->data_port += atoi(++p);
		}
	}

	if (p == NULL) {
		ftp->data_conn_mode = DCM_NOT_SET;
		return (ftp_send(ftp, "501 Can't interpret parameters\r\n"));
	}

	DEBUG_PRINT(ftp, "Data IP set to %u:%u:%u:%u\r\n", ip[0], ip[1], ip[2], ip[3]);
	DEBUG_PRINT(ftp, "Data port set to %u\r\n", ftp->data_port);

	IP4_ADDR(&ftp->ipclient, ip[0], ip[1], ip[2], ip[3]);
	ftp->data_conn_mode = DCM_ACTIVE;

	return (ftp_send(ftp, "200 PORT command successful\r\n"));
}

static ftp_result_t ftp_cmd_list(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	DIR dir;
	if (FTP_F_OPENDIR(&dir, ftp->path) != FR_OK) {
		return (ftp_send(ftp, "550 Can't open directory %s\r\n", ftp->parameters));
	}
	if (data_con_open(ftp) != FTP_RES_OK) {
		ftp_send(ftp, "425 Can't create connection\r\n");
		return (FTP_RES_ERROR);
	}
	if (ftp_send(ftp, "150 Accepted data connection\r\n") != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}

	while (FTP_F_READDIR(&dir, &ftp->finfo) == FR_OK) {
		if (ftp->finfo.fname[0] == 0) {
			break;
		}
		if (ftp->finfo.fname[0] == '.') {
			continue;
		}
		if (strcmp(ftp->command, "LIST")) {
			snprintf(ftp->ftp_buff, FTP_BUF_SIZE, "%s\r\n", ftp->finfo.fname);
		} else if (ftp->finfo.fattrib & AM_DIR) {
			snprintf(ftp->ftp_buff, FTP_BUF_SIZE, "+/,\t%s\r\n", ftp->finfo.fname);
		} else {
			snprintf(ftp->ftp_buff, FTP_BUF_SIZE, "+r,s%ld,\t%s\r\n", ftp->finfo.fsize, ftp->finfo.fname);
		}
		if (netconn_write(ftp->dataconn, ftp->ftp_buff, strlen(ftp->ftp_buff)) != FTP_RES_OK) {
			FTP_F_CLOSEDIR(&dir);
			data_con_close(ftp);
			return (FTP_RES_ERROR);
		}
	}

	FTP_F_CLOSEDIR(&dir);
	if (data_con_close(ftp) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}
	return (ftp_send(ftp, "226 Directory send OK.\r\n"));
}

static ftp_result_t ftp_cmd_mlsd(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	DIR dir;
	uint16_t nm = 0;

	if (FTP_F_OPENDIR(&dir, ftp->path) != FR_OK) {
		return (ftp_send(ftp, "550 Can't open directory %s\r\n", ftp->parameters));
	}
	if (data_con_open(ftp) != FTP_RES_OK) {
		ftp_send(ftp, "425 Can't create connection\r\n");
		return (FTP_RES_ERROR);
	}
	if (ftp_send(ftp, "150 Accepted data connection\r\n") != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}

	while (FTP_F_READDIR(&dir, &ftp->finfo) == FR_OK) {
		if (ftp->finfo.fname[0] == 0) {
			break;
		}
		if (ftp->finfo.fname[0] == '.') {
			continue;
		}
		if (ftp->finfo.fdate != 0) {
			snprintf(ftp->ftp_buff, FTP_BUF_SIZE, "Type=%s;Size=%ld;Modify=%s; %s\r\n", ftp->finfo.fattrib & AM_DIR ? "dir" : "file", ftp->finfo.fsize,
					data_time_to_str(ftp->date_str, ftp->finfo.fdate, ftp->finfo.ftime), ftp->finfo.fname);
		} else {
			snprintf(ftp->ftp_buff, FTP_BUF_SIZE, "Type=%s;Size=%ld; %s\r\n", ftp->finfo.fattrib & AM_DIR ? "dir" : "file", ftp->finfo.fsize, ftp->finfo.fname);
		}
		if (netconn_write(ftp->dataconn, ftp->ftp_buff, strlen(ftp->ftp_buff)) != FTP_RES_OK) {
			FTP_F_CLOSEDIR(&dir);
			data_con_close(ftp);
			return (FTP_RES_ERROR);
		}
		nm++;
	}

	FTP_F_CLOSEDIR(&dir);
	if (data_con_close(ftp) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}
	return (ftp_send(ftp, "226 Options: -a -l, %d matches total\r\n", nm));
}

static ftp_result_t ftp_cmd_dele(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "550 file %s not found\r\n", ftp->parameters));
	}

	if (FTP_F_UNLINK(ftp->path) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "450 Can't delete %s\r\n", ftp->parameters));
	}

	path_up_a_level(ftp->path);
	return (ftp_send(ftp, "250 Deleted %s\r\n", ftp->parameters));
}

static ftp_result_t ftp_cmd_noop(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	return (ftp_send(ftp, "200 Zzz...\r\n"));
}

static ftp_result_t ftp_cmd_retr(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "550 File %s not found\r\n", ftp->parameters));
	}
	if (FTP_F_OPEN(&ftp->file, ftp->path, FA_READ) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "450 Can't open %s\r\n", ftp->parameters));
	}
	if (data_con_open(ftp) != FTP_RES_OK) {
		FTP_F_CLOSE(&ftp->file);
		path_up_a_level(ftp->path);
		ftp_send(ftp, "425 Can't create connection\r\n");
		return (FTP_RES_ERROR);
	}
	DEBUG_PRINT(ftp, "Sending %s\r\n", ftp->parameters);
	if (ftp_send(ftp, "150 Connected to port %u, %lu bytes to download\r\n", ftp->data_port, FTP_F_SIZE(&ftp->file)) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}

	int bytes_transfered = 0;
	uint32_t bytes_read = 1;
	while (1) {
		if (FTP_F_READ(&ftp->file, ftp->ftp_buff, TCP_MSS, (UINT *) &bytes_read) != FR_OK) {
			if (ftp_send(ftp, "451 Communication error during transfer\r\n") != FTP_RES_OK) {
				FTP_F_CLOSE(&ftp->file);
				path_up_a_level(ftp->path);
				data_con_close(ftp);
				return (FTP_RES_ERROR);
			}
			break;
		}
		if (bytes_read == 0) {
			break;
		}
		if (netconn_write(ftp->dataconn, ftp->ftp_buff, bytes_read) != FTP_RES_OK) {
			FTP_F_CLOSE(&ftp->file);
			path_up_a_level(ftp->path);
			ftp_send(ftp, "426 Error during file transfer\r\n");
			data_con_close(ftp);
			return (FTP_RES_ERROR);
		}
		bytes_transfered += bytes_read;
	}

	DEBUG_PRINT(ftp, "Sent %u bytes\r\n", bytes_transfered);
	FTP_F_CLOSE(&ftp->file);
	path_up_a_level(ftp->path);
	if (data_con_close(ftp) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}
	return (ftp_send(ftp, "226 File successfully transferred\r\n"));
}

static ftp_result_t ftp_cmd_stor(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_OPEN(&ftp->file, ftp->path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "450 Can't open/create %s\r\n", ftp->parameters));
	}
	if (data_con_open(ftp) != 0) {
		FTP_F_CLOSE(&ftp->file);
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "425 Can't create connection\r\n"));
	}
	DEBUG_PRINT(ftp, "Receiving %s\r\n", ftp->parameters);
	netconn_set_recvtimeout(ftp->dataconn, FTP_STOR_RECV_TIMEOUT_MS);
	if (ftp_send(ftp, "150 Connected to port %u\r\n", ftp->data_port) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}

	uint32_t bytes_transfered = 0;
	uint32_t buff_free_bytes = FTP_BUF_SIZE;
	while (1) {
		struct pbuf *rcvbuf = NULL;
		int8_t con_err = netconn_recv_tcp_pbuf(ftp->dataconn, &rcvbuf);
		if (con_err == ERR_OK) {
			struct pbuf *rcvbuf_temp = rcvbuf;
			uint8_t *payload = (uint8_t*) (rcvbuf_temp->payload);
			FRESULT file_err = FR_OK;

			while (rcvbuf_temp != NULL) {
				bytes_transfered += rcvbuf_temp->len;

				if (rcvbuf_temp->len > FTP_BUF_SIZE) {
					uint32_t bytes_written = 0;
					file_err = FTP_F_WRITE(&ftp->file, payload, rcvbuf_temp->len, (UINT* ) &bytes_written);
					if (file_err != FR_OK) {
						break;
					}
					if (rcvbuf_temp->len != bytes_written) {
						file_err = FR_INT_ERR;
						break;
					}
				} else if (buff_free_bytes > rcvbuf_temp->len) {
					uint32_t used_bytes = FTP_BUF_SIZE - buff_free_bytes;
					memcpy(ftp->ftp_buff + used_bytes, payload, rcvbuf_temp->len);
					buff_free_bytes -= rcvbuf_temp->len;
				} else {
					uint32_t used_bytes = FTP_BUF_SIZE - buff_free_bytes;
					memcpy(ftp->ftp_buff + used_bytes, payload, buff_free_bytes);
					uint32_t bytes_written = 0;
					file_err = FTP_F_WRITE(&ftp->file, ftp->ftp_buff, FTP_BUF_SIZE, (UINT* ) &bytes_written);
					if (file_err != FR_OK) {
						break;
					}
					if (FTP_BUF_SIZE != bytes_written) {
						file_err = FR_INT_ERR;
						break;
					}
					uint32_t rest_bytes_to_save = rcvbuf_temp->len - buff_free_bytes;
					if (rest_bytes_to_save) {
						memcpy(ftp->ftp_buff, payload + buff_free_bytes, rest_bytes_to_save);
						buff_free_bytes = FTP_BUF_SIZE - rest_bytes_to_save;
					} else {
						buff_free_bytes = FTP_BUF_SIZE;
					}
				}
				rcvbuf_temp = rcvbuf_temp->next;
				payload = (uint8_t*) (rcvbuf_temp->payload);
			}
			pbuf_free(rcvbuf);
			if (file_err != 0) {
				if (ftp_send(ftp, "451 Communication error during transfer\r\n") != FTP_RES_OK) {
					FTP_F_CLOSE(&ftp->file);
					path_up_a_level(ftp->path);
					data_con_close(ftp);
					return (FTP_RES_ERROR);
				}
				break;
			}
		} else {
			FRESULT file_err = FR_OK;
			if (buff_free_bytes != FTP_BUF_SIZE) {
				uint32_t rest_bytes = FTP_BUF_SIZE - buff_free_bytes;
				uint32_t bytes_written = 0;
				file_err = FTP_F_WRITE(&ftp->file, ftp->ftp_buff, rest_bytes, (UINT* ) &bytes_written);
				if (rest_bytes != bytes_written) {
					file_err = FR_INT_ERR;
				}
			}
			if (file_err != 0) {
				if (ftp_send(ftp, "451 Communication error during transfer\r\n") != FTP_RES_OK) {
					FTP_F_CLOSE(&ftp->file);
					path_up_a_level(ftp->path);
					data_con_close(ftp);
					return (FTP_RES_ERROR);
				}
			}
			if (con_err != ERR_CLSD) {
				if (ftp_send(ftp, "426 Error during file transfer: %d\r\n", con_err) != FTP_RES_OK) {
					FTP_F_CLOSE(&ftp->file);
					path_up_a_level(ftp->path);
					data_con_close(ftp);
					return (FTP_RES_ERROR);
				}
			}
			break;
		}
	}

	DEBUG_PRINT(ftp, "Received %lu bytes\r\n", bytes_transfered);
	FTP_F_CLOSE(&ftp->file);
	path_up_a_level(ftp->path);

	if (data_con_close(ftp) != FTP_RES_OK) {
		return (FTP_RES_ERROR);
	}
	return (ftp_send(ftp, "226 File successfully transferred\r\n"));
}

static ftp_result_t ftp_cmd_mkd(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No directory name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(ftp->path, &ftp->finfo) == FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "521 \"%s\" directory already exists\r\n", ftp->parameters));
	}

	if (FTP_F_MKDIR(ftp->path) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "550 Can't create \"%s\"\r\n", ftp->parameters));
	}

	DEBUG_PRINT(ftp, "Creating directory %s\r\n", ftp->parameters);
	return (ftp_send(ftp, "257 \"%s\" created\r\n", ftp->parameters));
}

static ftp_result_t ftp_cmd_rmd(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No directory name\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	DEBUG_PRINT(ftp, "Deleting %s\r\n", ftp->path);

	if (FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "550 Directory \"%s\" not found\r\n", ftp->parameters));
	}

	if (FTP_F_UNLINK(ftp->path) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "501 Can't delete \"%s\"\r\n", ftp->parameters));
	}
	path_up_a_level(ftp->path);
	return (ftp_send(ftp, "250 \"%s\" removed\r\n", ftp->parameters));
}

static ftp_result_t ftp_cmd_rnfr(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (strlen(ftp->parameters) == 0) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	memcpy(ftp->path_rename, ftp->path, FTP_CWD_SIZE);
	if (!path_build(ftp->path_rename, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(ftp->path_rename, &ftp->finfo) != FR_OK) {
		return (ftp_send(ftp, "550 file \"%s\" not found\r\n", ftp->parameters));
	}
	DEBUG_PRINT(ftp, "Renaming %s\r\n", ftp->path_rename);
	return (ftp_send(ftp, "350 RNFR accepted - file exists, ready for destination\r\n"));
}

static ftp_result_t ftp_cmd_rnto(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	if (!strlen(ftp->parameters)) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	if (!strlen(ftp->path_rename)) {
		return (ftp_send(ftp, "503 Need RNFR before RNTO\r\n"));
	}
	if (!path_build(ftp->path, ftp->parameters)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}

	if (FTP_F_STAT(ftp->path, &ftp->finfo) == FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "553 \"%s\" already exists\r\n", ftp->parameters));
	}

	DEBUG_PRINT(ftp, "Renaming %s to %s\r\n", ftp->path_rename, ftp->path);
	if (FTP_F_RENAME(ftp->path_rename, ftp->path) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "451 Rename/move failure\r\n"));
	} else {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "250 File successfully renamed or moved\r\n"));
	}
}

static ftp_result_t ftp_cmd_feat(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	return (ftp_send(ftp, "211 Extensions supported:\r\n MDTM\r\n MLSD\r\n SIZE\r\n SITE FREE\r\n211 End.\r\n"));
}

static ftp_result_t ftp_cmd_syst(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}
	return (ftp_send(ftp, "215 FTP Server, V1.0\r\n"));
}

static ftp_result_t ftp_cmd_mdtm(ftp_data_t *ftp) {
	if (!FTP_IS_LOGGED_IN(ftp)) {
		return (FTP_RES_OK);
	}

	uint16_t date;
	uint16_t time;
	uint8_t gettime = date_time_get(ftp->parameters, &date, &time);
	char *fname = ftp->parameters + gettime;

	if (strlen(fname) == 0) {
		return (ftp_send(ftp, "501 No file name\r\n"));
	}
	if (!path_build(ftp->path, fname)) {
		return (ftp_send(ftp, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK) {
		path_up_a_level(ftp->path);
		return (ftp_send(ftp, "550 file \"%s\" not found\r\n", ftp->parameters));
	}

	path_up_a_level(ftp->path);
	if (!gettime) {
		return (ftp_send(ftp, "213 %s\r\n", data_time_to_str(ftp->date_str, ftp->finfo.fdate, ftp->finfo.ftime)));
	}

	ftp->finfo.fdate = date;
	ftp->finfo.ftime = time;
	if (FTP_F_UTIME(ftp->path, &ftp->finfo) == FR_OK) {
		return (ftp_send(ftp, "200 Ok\r\n"));
	} else {
		return (ftp_send(ftp, "550 Unable to modify time\r\n"));
	}
}

static ftp_result_t ftp_cmd_size(ftp_data_t *ftp) {
	// are we not yet logged in?
	if (!FTP_IS_LOGGED_IN(ftp))
		return;

	if (strlen(ftp->parameters) == 0) {
		ftp_send(ftp, "501 No file name\r\n");
		return;
	}

	if (!path_build(ftp->path, ftp->parameters)) {
		ftp_send(ftp, "500 Command line too long\r\n");
		return;
	}

	if (FTP_F_STAT(ftp->path, &ftp->finfo) != FR_OK || (ftp->finfo.fattrib & AM_DIR)) {
		// send error to client
		ftp_send(ftp, "550 No such file\r\n");
	} else {
		ftp_send(ftp, "213 %lu\r\n", ftp->finfo.fsize);
		FTP_F_CLOSE(&ftp->file);
	}

	// go up a level again
	path_up_a_level(ftp->path);
}

static void ftp_cmd_site(ftp_data_t *ftp) {
	// are we not yet logged in?
	if (!FTP_IS_LOGGED_IN(ftp))
		return;

	if (!strcmp(ftp->parameters, "FREE")) {
		FATFS *fs;
		uint32_t free_clust;
		FTP_F_GETFREE("0:", &free_clust, &fs);
		ftp_send(ftp, "211 %lu MB free of %lu MB capacity\r\n", free_clust * fs->csize >> 11, (fs->n_fatent - 2) * fs->csize >> 11);
	} else {
		ftp_send(ftp, "550 Unknown SITE command %s\r\n", ftp->parameters);
	}
}

static void ftp_cmd_stat(ftp_data_t *ftp) {
	// are we not yet logged in?
	if (!FTP_IS_LOGGED_IN(ftp))
		return;

	// print status
	ftp_send(ftp, "221 FTP Server status: you will be disconnected after %d minutes of inactivity\r\n",
			(FTP_SERVER_INACTIVE_CNT * FTP_SERVER_READ_TIMEOUT_MS) / 60000);
}

static void ftp_cmd_auth(ftp_data_t *ftp) {
	// no tls or ssl available
	ftp_send(ftp, "504 Not available\r\n");
}

static void ftp_cmd_user(ftp_data_t *ftp) {
	// is this the normal user that is trying to log in?
	if (FTP_USER_NAME_OK(ftp->parameters)) {
		// all good
		ftp_send(ftp, "331 OK. Password required\r\n");

		// waiting for user password
		ftp->user = FTP_USER_USER_NO_PASS;
	}
	// unknown user
	else {
		// not a user and not an admin, error
		ftp_send(ftp, "530 Username not known\r\n");
	}
}

static void ftp_cmd_pass(ftp_data_t *ftp) {
	// in idle state?
	if (ftp->user == FTP_USER_NONE) {
		// user not specified
		ftp_send(ftp, "530 User not specified\r\n");
	}
	// is this the normal user that is trying to log in?
	else if (FTP_USER_PASS_OK(ftp->parameters)) {
		// username and password accepted
		ftp_send(ftp, "230 OK, logged in as user\r\n");

		// user enabled
		ftp->user = FTP_USER_USER_LOGGED_IN;
	}
	// unknown password
	else {
		// error, return
		ftp_send(ftp, "530 Password not correct\r\n");
	}
}

static ftp_cmd_t ftpd_commands[] = { //
		{ "PWD", ftp_cmd_pwd }, //
		{ "CWD", ftp_cmd_cwd }, //
		{ "CDUP", ftp_cmd_cdup }, //
		{ "MODE", ftp_cmd_mode }, //
		{ "STRU", ftp_cmd_stru }, //
		{ "TYPE", ftp_cmd_type }, //
		{ "PASV", ftp_cmd_pasv }, //
		{ "PORT", ftp_cmd_port }, //
		{ "NLST", ftp_cmd_list }, //
		{ "LIST", ftp_cmd_list }, //
		{ "MLSD", ftp_cmd_mlsd }, //
		{ "DELE", ftp_cmd_dele }, //
		{ "NOOP", ftp_cmd_noop }, //
		{ "RETR", ftp_cmd_retr }, //
		{ "STOR", ftp_cmd_stor }, //
		{ "MKD", ftp_cmd_mkd }, //
		{ "RMD", ftp_cmd_rmd }, //
		{ "RNFR", ftp_cmd_rnfr }, //
		{ "RNTO", ftp_cmd_rnto }, //
		{ "FEAT", ftp_cmd_feat }, //
		{ "MDTM", ftp_cmd_mdtm }, //
		{ "SIZE", ftp_cmd_size }, //
		{ "SITE", ftp_cmd_site }, //
		{ "STAT", ftp_cmd_stat }, //
		{ "SYST", ftp_cmd_syst }, //
		{ "AUTH", ftp_cmd_auth }, //
		{ "USER", ftp_cmd_user }, //
		{ "PASS", ftp_cmd_pass }, //
		{ NULL, NULL } //
		};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//			process a command
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static ftp_result_t ftp_process_command(ftp_data_t *ftp, bool *quit) {
	if (!strcmp(ftp->command, "QUIT")) {
		*quit = true;
		return (ftp_send(ftp, "221 Goodbye\r\n"));
	}
	ftp_cmd_t *cmd = ftpd_commands;
	while (cmd->cmd != NULL && cmd->func != NULL) {
		if (!strcmp(cmd->cmd, ftp->command)) {
			break;
		}
		cmd++;
	}
	if (cmd->cmd != NULL && cmd->func != NULL) {
		FTP_CMD_BEGIN_CALLBACK(cmd->cmd);
		ftp_result_t res = cmd->func(ftp);
		FTP_CMD_END_CALLBACK(cmd->cmd);
		return (res);
	} else {
		return (ftp_send(ftp, "500 Unknown command\r\n"));
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//			Main FTP server
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Service the FTP server connection that is accepted.
 *
 * @param ctrlcn Connection that was created for FTP server
 * @param ftp The FTP structure containing all variables
 */
static void ftp_service(struct netconn *ctrlcn, ftp_data_t *ftp, bool *stop) {
	uint16_t dummy;
	ip4_addr_t ippeer;

	// reset the working directory to root
	strncpy(ftp->path, "/", FTP_CWD_SIZE);
	memset(ftp->path_rename, 0, FTP_CWD_SIZE);

	// variables initialization
	ftp->ctrlconn = ctrlcn;
	ftp->listdataconn = NULL;
	ftp->dataconn = NULL;
	ftp->data_port = 0;
	ftp->data_conn_mode = DCM_NOT_SET;
	ftp->user = FTP_USER_NONE;

	// bugfix which works around ports which are already in use (from a previous connection)
	ftp->data_port_incremented = (ftp->data_port_incremented + 1) % PORT_INCREMENT_OFFSET;

	//  Get the local and peer IP
	netconn_addr(ftp->ctrlconn, &ftp->ipserver, &dummy);
	netconn_peer(ftp->ctrlconn, &ippeer, &dummy);
	// Set disconnection timeout to one second
	netconn_set_recvtimeout(ftp->ctrlconn, FTP_SERVER_READ_TIMEOUT_MS);
	netconn_set_sendtimeout(ftp->ctrlconn, FTP_SERVER_WRITE_TIMEOUT_MS);

	// send welcome message
	if (ftp_send(ftp, "220 -> CMS FTP Server, FTP Version %s\r\n", FTP_VERSION) == FTP_RES_OK) {
		DEBUG_PRINT(ftp, "Client connected!\r\n");
		bool quit = false;
		while (1) {
			if (ftp_read_command(ftp, stop) != FTP_RES_OK) {
				break;
			}
			if (ftp_parse_command(ftp) != FTP_RES_OK) {
				break;
			}
			if (ftp_process_command(ftp, &quit) != FTP_RES_OK) {
				break;
			}
			if (quit) {
				break;
			}
		}
	}

	pasv_con_close(ftp);
	data_con_close(ftp);
	DEBUG_PRINT(ftp, "Client disconnected\r\n");
}

// single ftp connection loop
static void ftp_task(void *param) {
	if (param == NULL) {
		FTP_CRITICAL_ERROR_HANDLER();
	}
	server_stru_t *ftp = (server_stru_t*) param;
	ftp->ftp_data.ftp_con_num = ftp->number;
	ftp->busy = false;

	while (1) {
		if (ftp->ftp_connection != NULL) {
			ftp->busy = true;
			FTP_CONNECTED_CALLBACK();
			FTP_LOG_PRINT("FTP %d connected\r\n", ftp->number);
			ftp_service(ftp->ftp_connection, &ftp->ftp_data, &ftp->stop);
			if (netconn_delete(ftp->ftp_connection) != ERR_OK) {
				FTP_LOG_PRINT("server NETCONN delete error\r\n");
				ftp_set_error(FTP_ERROR_CLIENT_NETCONN_DELETE);
			}
			ftp->ftp_connection = NULL;
			FTP_LOG_PRINT("FTP %d disconnected\r\n", ftp->number);
			FTP_DISCONNECTED_CALLBACK();
			ftp->busy = false;
		} else {
			vTaskDelay(500);
		}
	}
}

/**
 * @brief start FTP server
 */
void ftp_start(void) {
	if (FTP.status == FTP_IDLE || FTP.status == FTP_ERROR) {
		FTP.status = FTP_STARTING;
	}
}

/**
 * @brief STOP FTP server, it can take a while
 */
void ftp_stop(void) {
	if (FTP.status == FTP_RUNNING) {
		FTP.status = FTP_STOPPING;
	}
}

/**
 * @brief get FTP errors
 * @return FTP errors
 */
uint32_t ftp_get_errors(void) {
	return (FTP.errors);
}

/**
 * @brief clear FTP errors
 */
void ftp_clear_errors(void) {
	if (FTP.status == FTP_ERROR) {
		FTP.errors = 0;
	}
}

static struct netconn* ftp_starting(void) {
	struct netconn *ftp_srv_conn = netconn_new(NETCONN_TCP);
	if (ftp_srv_conn == NULL) {
		FTP_LOG_PRINT("Failed to create socket\r\n");
		ftp_set_error(FTP_ERROR_SERVER_NETCONN_NEW);
	} else if (FTP.port == 0) {
		FTP_LOG_PRINT("Port is 0\r\n");
		ftp_set_error(FTP_ERROR_PORT_IS_ZERO);
	} else if (netconn_bind(ftp_srv_conn, NULL, FTP.port) != ERR_OK) {
		FTP_LOG_PRINT("Can not bin to port\r\n");
		ftp_set_error(FTP_ERROR_BIND_TO_PORT);
	} else if (netconn_listen(ftp_srv_conn) != ERR_OK) {
		FTP_LOG_PRINT("Can not listen on this NETCONN\r\n");
		ftp_set_error(FTP_ERROR_SERVER_NETCONN_LISTEN);
	} else {
		netconn_set_recvtimeout(ftp_srv_conn, FTP_PSV_ACCEPT_TIMEOUT_MS);
		FTP.status = FTP_RUNNING;
	}
	return (ftp_srv_conn);
}

static void ftp_running(struct netconn *ftp_srv_conn) {
	struct netconn *ftp_client_conn = NULL;
	if (netconn_accept(ftp_srv_conn, &ftp_client_conn) == ERR_OK) {
		uint8_t index = 0;
		for (index = 0; index < FTP_NBR_CLIENTS; index++) {
			if (ftp_links[index].ftp_connection == NULL && ftp_links[index].busy == false) {
				break;
			}
		}
		if (index >= FTP_NBR_CLIENTS) {
			FTP_LOG_PRINT("FTP connection denied, all connections in use\r\n");
			netconn_set_recvtimeout(ftp_client_conn, FTP_SERVER_READ_TIMEOUT_MS);
			netconn_set_sendtimeout(ftp_client_conn, FTP_SERVER_WRITE_TIMEOUT_MS);
			err_t err = netconn_write(ftp_client_conn, no_conn_allowed, strlen(no_conn_allowed));
			if (err != ERR_OK && err != ERR_TIMEOUT) {
				FTP_LOG_PRINT("client NETCONN write error\r\n");
				ftp_set_error(FTP_ERROR_CLIENT_NETCONN_WRITE);
			}
			if (netconn_delete(ftp_client_conn) != ERR_OK) {
				FTP_LOG_PRINT("client NETCONN delete error\r\n");
				ftp_set_error(FTP_ERROR_CLIENT_NETCONN_DELETE);
			}
			vTaskDelay(500);
		} else {
			ftp_links[index].stop = false;
			ftp_links[index].ftp_connection = ftp_client_conn;
		}
	}
}

static void ftp_stopping(struct netconn *ftp_srv_conn) {
	if (netconn_delete(ftp_srv_conn) != ERR_OK) {
		FTP_LOG_PRINT("server NETCONN delete error\r\n");
		ftp_set_error(FTP_ERROR_SERVER_NETCONN_DELETE);
	}
	for (uint8_t index = 0; index < FTP_NBR_CLIENTS; ++index) {
		if (ftp_links[index].busy) {
			ftp_links[index].stop = true;
		}
	}
	bool all_tasks_disable = false;
	for (uint8_t cnt = 0; cnt < 6; ++cnt) {
		vTaskDelay(1000);
		bool some_task_is_still_running = false;
		for (uint8_t index = 0; index < FTP_NBR_CLIENTS; ++index) {
			if (ftp_links[index].busy) {
				some_task_is_still_running = true;
				break;
			}
		}
		if (some_task_is_still_running == false) {
			all_tasks_disable = true;
			break;
		}
	}
	if (!all_tasks_disable) {
		FTP_LOG_PRINT("Can not disable all FTP tasks\r\n");
		ftp_set_error(FTP_ERROR_NOT_ALL_TASK_DISABLED);
	}
}

/**
 * @brief FTP server task
 * @param argument not used
 */
static void ftp_server(void *argument) {
	UNUSED(argument);
	struct netconn *ftp_srv_conn;

	while (1) {
		switch (FTP.status) {
		case FTP_IDLE:
			vTaskDelay(1000);
			break;
		case FTP_STARTING:
			ftp_srv_conn = ftp_starting();
			break;
		case FTP_RUNNING:
			ftp_running(ftp_srv_conn);
			break;
		case FTP_STOPPING:
			ftp_stopping(ftp_srv_conn);
			if (FTP.status == FTP_STOPPING) {
				FTP.status = FTP_IDLE;
			}
			break;
		case FTP_ERROR_STOPPING:
			ftp_stopping(ftp_srv_conn);
			FTP.status = FTP_ERROR;
			break;
		case FTP_ERROR:
			vTaskDelay(1000);
			break;
		default:
			vTaskDelay(1000);
			break;
		}
	}
}

/**
 * @brief init all tasks
 * call this before kernel start
 */
void ftp_init(void) {
	if (!FTP.inited) {
		FTP.inited = true;
		FTP.stats.clients_max = FTP_NBR_CLIENTS;

		char name[configMAX_TASK_NAME_LEN + 1] = { 0 };
		for (uint8_t index = 0; index < FTP_NBR_CLIENTS; ++index) {
			server_stru_t *data = &ftp_links[index];
			ftp_links[index].number = index;
			snprintf(name, configMAX_TASK_NAME_LEN, "ftp_client_%d", data->number);
#if FTP_CLIENT_TASK_STATIC == 1
			data->task_handle = xTaskCreateStatic(ftp_task, name, FTP_TASK_STACK_SIZE, data, FTP_TASK_PRIORITY, data->task_stack, &data->task_static);
			if (data->task_handle == NULL) {
				FTP_CRITICAL_ERROR_HANDLER();
			}
#else
			if (xTaskCreate(ftp_task, name, FTP_CLIENT_TASK_STACK_SIZE, data, FTP_CLIENT_TASK_PRIORITY, &data->task_handle) != pdPASS) {
				FTP_CRITICAL_ERROR_HANDLER();
			}
#endif
		}

		if (xTaskCreate(ftp_server, "ftp_server", FTP_SERVER_TASK_STACK_SIZE, NULL, FTP_SERVER_TASK_PRIORITY, &FTP.server_task_handle) != pdPASS) {
			FTP_CRITICAL_ERROR_HANDLER();
		}
	}
}

/**
 * @brief set new user name
 * @param name new user name
 */
void ftp_set_username(const char *name) {
	if (name == NULL)
		return;
	strncpy(ftp_user_name, name, FTP_USER_NAME_LEN);
}

/**
 * @brief set new password
 * @param pass new password
 */
void ftp_set_password(const char *pass) {
	if (pass == NULL)
		return;
	strncpy(ftp_user_pass, pass, FTP_USER_PASS_LEN);
}

/**
 * @brief get current status
 * @return current status
 */
ftp_status_t ftp_get_status(void) {
	return (FTP.status);
}

/**
 * @brief set current port
 * FTP must be restarted to apply new port
 * @param port NETCONN TCP port
 */
void ftp_set_port(uint16_t port) {
	FTP.port = port;
}

/**
 * @brief get current port
 * @return current port
 */
uint16_t ftp_get_port(void) {
	return (FTP.port);
}

const ftp_stats_t* ftp_get_stats(void) {
	return (&FTP.stats);
}
