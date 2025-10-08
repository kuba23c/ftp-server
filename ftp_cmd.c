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
#include "ftp_client.h"
#include "ftp_pasv.h"
#include "ftp_active.h"

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
	ftp_data_msg_t msg;
} ftp_cmd_temp_t;

typedef struct {
	uint8_t index;
	char *command;
	char *parameters;
	uint16_t parameters_len;
	ftp_cmd_temp_t temp;
	struct pbuf *p;
	char *path_rename;
} ftp_cmd_handler_data_t;

typedef struct {
	const char *cmd;
	ftp_result_t (*func)(ftp_cmd_handler_data_t *const data);
} ftp_cmd_handlers_t;

typedef struct {
	char *command;
	uint16_t command_len;
	char *parameters;
	uint16_t parameters_len;
	struct pbuf *p;
	char path_rename[FTP_CWD_SIZE];
} ftp_cmd_t;

static void set_path_to_root(char *path) {
	strncpy(path, "/", FTP_CWD_SIZE);
}

void path_up_a_level(char *path) {
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

static ftp_result_t ftp_cmd_noop(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	return (ftp_cmd_resp_send(data->index, "200 zzz...\r\n"));
}

static ftp_result_t ftp_cmd_quit(ftp_cmd_handler_data_t *const data) {
	return (ftp_cmd_resp_send(data->index, "221 Goodbye\r\n"));
}

// print working directory
static ftp_result_t ftp_cmd_pwd(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	} else {
		return (ftp_cmd_resp_send(data->index, "257 \"%s\" is your current directory\r\n", ftp_get_path(data->index)));
	}
}

// change working directory
static ftp_result_t ftp_cmd_cwd(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No directory name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	if (strcmp(data->temp.path, "/") != 0 && FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK) {
		return (ftp_cmd_resp_send(data->index, "550 Failed to change directory to %s\r\n", data->temp.path));
	}

	return (ftp_cmd_resp_send(data->index, "250 Directory successfully changed.\r\n"));
}

// Change the remote machine working directory to the parent of the current remote machine working directory.
static ftp_result_t ftp_cmd_cdup(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	set_path_to_root(ftp_get_path(data->index));
	return (ftp_cmd_resp_send(data->index, "250 Directory successfully changed to root.\r\n"));
}

// change mode
static ftp_result_t ftp_cmd_mode(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "S")) {
		return (ftp_cmd_resp_send(data->index, "200 S OK\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "504 Only S(tream) is supported\r\n"));
	}
}

static ftp_result_t ftp_cmd_stru(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "F")) {
		return (ftp_cmd_resp_send(data->index, "200 F OK\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "504 Only F(ile) is supported\r\n"));
	}
}

static ftp_result_t ftp_cmd_type(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	if (!strcmp(data->parameters, "A")) {
		return (ftp_cmd_resp_send(data->index, "200 TYPE is now ASCII\r\n"));
	} else if (!strcmp(data->parameters, "I")) {
		return (ftp_cmd_resp_send(data->index, "200 TYPE is now 8-bit binary\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "504 Unknown TYPE\r\n"));
	}
}

// set passive data connection
static ftp_result_t ftp_cmd_pasv(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
#if FTP_USE_PASSIVE_MODE == 1
	if (ftp_pasv_start(data->index)) {

		return (FTP_RES_OK);
	} else {
		ftp_cmd_resp_send(data->index, "425 Can't set connection management to passive\r\n");
		return (FTP_RES_ERROR);
	}
#else
	return (ftp_cmd_resp_send(data->index, "421 Passive mode not available\r\n"));
#endif
}

// set active data connection
static ftp_result_t ftp_cmd_port(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 no parameters given\r\n"));
	}

	// Start building IP
	uint8_t ip[4] = { 0 };
	uint8_t i = 0;
	uint16_t port = 0;

	char *p = data->parameters - 1;
	for (i = 0; i < 4 && p != NULL; i++) {
		if (p == NULL) {
			break;
		}
		ip[i] = atoi(++p);
		p = strchr(p, ',');
	}

	if (p != NULL) {
		if (i == 4) {
			port = 256 * atoi(++p);
		}
		p = strchr(p, ',');
		if (p != NULL) {
			port += atoi(++p);
		}
	}

	if (p == NULL) {
		return (ftp_cmd_resp_send(data->index, "501 Can't interpret parameters\r\n"));
	}

	DEBUG_PRINT(data->index, "Data IP set to %u:%u:%u:%u\r\n", ip[0], ip[1], ip[2], ip[3]);
	DEBUG_PRINT(data->index, "Data port set to %u\r\n", port);

	ftp_active_set_ip(data->index, ip[0], ip[1], ip[2], ip[3]);
	ftp_active_set_port(data->index, port);
	ftp_set_data_conn_mode(data->index, DCM_ACTIVE);

	return (ftp_cmd_resp_send(data->index, "200 PORT command successful\r\n"));
}

static ftp_result_t data_con_open(ftp_cmd_handler_data_t *const data) {
	dcm_type mode = ftp_get_data_conn_mode(data->index);
	if (mode == DCM_PASSIVE) {
		return (FTP_RES_OK);
	} else if (mode == DCM_ACTIVE) {
		err_t err = ftp_active_connect(data->index);
		if (err == ERR_OK) {
			DEBUG_PRINT(data->index, "Starting Active connection\r\n");
			return (FTP_RES_OK);
		} else {
			DEBUG_PRINT(data->index, "Failed to start Active connection\r\n");
			return (FTP_RES_ERROR);
		}
	} else {
		return (FTP_RES_ERROR);
	}
}

static ftp_result_t ftp_cmd_list(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	if (data_con_open(data) != FTP_RES_OK) {
		ftp_cmd_resp_send(data->index, "425 Can't create connection\r\n");
		return (FTP_RES_ERROR);
	}
	pbuf_ref(data->p);
	data->temp.msg.msg_type = FTP_DATA_MSG_LIST;
	data->temp.msg.index = data->index;
	data->temp.msg.data.list.p = data->p;
	data->temp.msg.data.list.command = data->command;
	data->temp.msg.data.list.parameters = data->parameters;
	ftp_data_handle(&(data->temp.msg));
	DEBUG_PRINT(data->index, "Sending list %s\r\n", data->parameters);
	ftp_cmd_resp_send(data->index, "150 Accepted data connection\r\n");
	return (FTP_RES_OK);
}

static ftp_result_t ftp_cmd_retr(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}

	if (data_con_open(data) != FTP_RES_OK) {
		path_up_a_level(data->temp.path);
		ftp_cmd_resp_send(data->index, "425 Can't create connection\r\n");
		return (FTP_RES_ERROR);
	}
	pbuf_ref(data->p);
	data->temp.msg.msg_type = FTP_DATA_MSG_START_TX;
	data->temp.msg.index = data->index;
	data->temp.msg.data.tx.p = data->p;
	data->temp.msg.data.tx.parameters = data->parameters;
	data->temp.msg.data.tx.path = data->temp.path;
	ftp_data_handle(&(data->temp.msg));
	DEBUG_PRINT(data->index, "Sending file %s\r\n", data->parameters);
	ftp_cmd_resp_send(data->index, "150 Accepted data connection\r\n");
	return (FTP_RES_OK);
}

static ftp_result_t ftp_cmd_dele(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "550 file %s not found\r\n", data->parameters));
	}

	if (FTP_F_UNLINK(data->temp.path) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "450 Can't delete %s\r\n", data->parameters));
	}

	path_up_a_level(data->temp.path);
	return (ftp_cmd_resp_send(data->index, "250 Deleted %s\r\n", data->parameters));
}

static ftp_result_t ftp_cmd_mkd(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No directory name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) == FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "521 \"%s\" directory already exists\r\n", data->parameters));
	}

	if (FTP_F_MKDIR(data->temp.path) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "550 Can't create \"%s\"\r\n", data->parameters));
	}

	DEBUG_PRINT(data->index, "Creating directory %s\r\n", data->parameters);
	return (ftp_cmd_resp_send(data->index, "257 \"%s\" created\r\n", data->parameters));
}

static ftp_result_t ftp_cmd_rmd(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No directory name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	DEBUG_PRINT(data->index, "Deleting %s\r\n", data->temp.path);

	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "550 Directory \"%s\" not found\r\n", data->parameters));
	}

	if (FTP_F_UNLINK(data->temp.path) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "501 Can't delete \"%s\"\r\n", data->parameters));
	}
	path_up_a_level(data->temp.path);
	return (ftp_cmd_resp_send(data->index, "250 \"%s\" removed\r\n", data->parameters));
}

static ftp_result_t ftp_cmd_rnfr(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	memcpy(data->path_rename, data->temp.path, FTP_CWD_SIZE);
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->path_rename, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(data->path_rename, &(data->temp.finfo)) != FR_OK) {
		return (ftp_cmd_resp_send(data->index, "550 file \"%s\" not found\r\n", data->parameters));
	}
	DEBUG_PRINT(data->index, "Renaming %s\r\n", data->path_rename);
	return (ftp_cmd_resp_send(data->index, "350 RNFR accepted - file exists, ready for destination\r\n"));
}

static ftp_result_t ftp_cmd_rnto(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (!data->parameters_len) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	if (!strlen(data->path_rename)) {
		return (ftp_cmd_resp_send(data->index, "503 Need RNFR before RNTO\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}

	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) == FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "553 \"%s\" already exists\r\n", data->parameters));
	}

	DEBUG_PRINT(data->index, "Renaming %s to %s\r\n", data->path_rename, data->temp.path);
	if (FTP_F_RENAME(data->path_rename, data->temp.path) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "451 Rename/move failure\r\n"));
	} else {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "250 File successfully renamed or moved\r\n"));
	}
}

static ftp_result_t ftp_cmd_feat(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	return (ftp_cmd_resp_send(data->index, "211 Extensions supported:\r\n MDTM\r\n MLSD\r\n SIZE\r\n SITE FREE\r\n211 End.\r\n"));
}

static ftp_result_t ftp_cmd_syst(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	return (ftp_cmd_resp_send(data->index, "215 FTP Server, V1.0\r\n"));
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

static ftp_result_t ftp_cmd_mdtm(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	uint16_t date;
	uint16_t time;
	uint8_t gettime = date_time_get(data->parameters, &date, &time);
	char *fname = data->parameters + gettime;

	if (strlen(fname) == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, fname, strlen(fname))) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}
	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "550 file \"%s\" not found\r\n", data->parameters));
	}

	path_up_a_level(data->temp.path);
	if (!gettime) {
		return (ftp_cmd_resp_send(data->index, "213 %s\r\n", data_time_to_str(data->path_rename, data->temp.finfo.fdate, data->temp.finfo.ftime)));
	}

	data->temp.finfo.fdate = date;
	data->temp.finfo.ftime = time;
	if (FTP_F_UTIME(data->temp.path, &(data->temp.finfo)) == FR_OK) {
		return (ftp_cmd_resp_send(data->index, "200 Ok\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "550 Unable to modify time\r\n"));
	}
}

static ftp_result_t ftp_cmd_size(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (data->parameters_len == 0) {
		return (ftp_cmd_resp_send(data->index, "501 No file name\r\n"));
	}
	data->temp.path = ftp_get_path(data->index);
	if (!path_build(data->temp.path, data->parameters, data->parameters_len)) {
		return (ftp_cmd_resp_send(data->index, "500 Command line too long\r\n"));
	}

	if (FTP_F_STAT(data->temp.path, &(data->temp.finfo)) != FR_OK || (data->temp.finfo.fattrib & AM_DIR)) {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "550 No such file\r\n"));
	} else {
		path_up_a_level(data->temp.path);
		return (ftp_cmd_resp_send(data->index, "213 %lu\r\n", data->temp.finfo.fsize));
	}
}

static ftp_result_t ftp_cmd_site(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}

	if (!strcmp(data->parameters, "FREE")) {
		FATFS *fs;
		uint32_t free_clust;
		FTP_F_GETFREE("0:", &free_clust, &fs);
		return (ftp_cmd_resp_send(data->index, "211 %lu MB free of %lu MB capacity\r\n", free_clust * fs->csize >> 11, (fs->n_fatent - 2) * fs->csize >> 11));
	} else {
		return (ftp_cmd_resp_send(data->index, "550 Unknown SITE command %s\r\n", data->parameters));
	}
}

static ftp_result_t ftp_cmd_stat(ftp_cmd_handler_data_t *const data) {
	if (!ftp_is_logged_in(data->index)) {
		return (FTP_RES_OK);
	}
	return (ftp_cmd_resp_send(data->index, "221 FTP Server status: you will be disconnected after %d minutes of inactivity\r\n",
			(FTP_SERVER_INACTIVE_CNT * FTP_SERVER_READ_TIMEOUT_MS) / 60000));
}

static ftp_result_t ftp_cmd_auth(ftp_cmd_handler_data_t *const data) {
	return (ftp_cmd_resp_send(data->index, "504 Not available\r\n"));
}

static ftp_result_t ftp_cmd_user(ftp_cmd_handler_data_t *const data) {
	if (ftp_is_user_name_ok(data->parameters)) {
		ftp_set_user(data->index, FTP_USER_USER_NO_PASS);
		return (ftp_cmd_resp_send(data->index, "331 OK. Password required\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "530 Username not known\r\n"));
	}
}

static ftp_result_t ftp_cmd_pass(ftp_cmd_handler_data_t *const data) {
	if (ftp_get_user(data->index) == FTP_USER_NONE) {
		return (ftp_cmd_resp_send(data->index, "530 User not specified\r\n"));
	} else if (ftp_is_pass_ok(data->parameters)) {
		ftp_set_user(data->index, FTP_USER_USER_LOGGED_IN);
		return (ftp_cmd_resp_send(data->index, "230 OK, logged in as user\r\n"));
	} else {
		return (ftp_cmd_resp_send(data->index, "530 Password not correct\r\n"));
	}
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
		{ "PORT", ftp_cmd_port }, //
		{ "NLST", ftp_cmd_list }, //
		{ "LIST", ftp_cmd_list }, //
//		{ "MLSD", ftp_cmd_mlsd }, //
		{ "DELE", ftp_cmd_dele }, //
		{ "RETR", ftp_cmd_retr }, //
//		{ "STOR", ftp_cmd_stor }, //
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

static ftp_result_t ftp_process_command(uint8_t index, ftp_cmd_t *const ftp_cmd) {
	ftp_cmd_handlers_t *handler = ftpd_commands;
	uint16_t cmd_len = 0;
	while (handler->cmd != NULL && handler->func != NULL) {
		cmd_len = strlen(handler->cmd);
		if (cmd_len == ftp_cmd->command_len) {
			if (!strncmp(handler->cmd, ftp_cmd->command, cmd_len)) {
				ftp_cmd_handler_data_t data = { .index = index, .command = ftp_cmd->command, .parameters = ftp_cmd->parameters, .parameters_len =
						ftp_cmd->parameters_len, .p = ftp_cmd->p, .path_rename = ftp_cmd->path_rename };
				return (handler->func(&data));
			}
		}
		handler++;
	}
	return (ftp_cmd_resp_send(index, "500 Unknown command\r\n"));
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
			DEBUG_PRINT(msg.index, "Incomming: %.*s %.*s\r\n", ftp_cmd.command_len, ftp_cmd.command, ftp_cmd.parameters_len, ftp_cmd.parameters);
			ftp_cmd.p = msg.p;
			if (ftp_process_command(msg.index, &ftp_cmd) != FTP_RES_OK) {
				DEBUG_PRINT(msg.index, "CMD process: FAILED\r\n");
			} else {
				DEBUG_PRINT(msg.index, "CMD process: SUCCESS\r\n");
			}
		} else {
			DEBUG_PRINT(msg.index, "Wrong command: %.*s\r\n", msg.p->len, msg.p->payload);
		}
		pbuf_free(msg.p);
		memset(&ftp_cmd, 0, sizeof(ftp_cmd_t));
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
	assert_param(ftp_cmd_task_handle != NULL);
}

