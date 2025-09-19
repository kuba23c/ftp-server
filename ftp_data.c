/*
 * ftp_data.c
 *
 *  Created on: Sep 17, 2025
 *      Author: jakubczekaj
 */

#include "ftp_data.h"
#include "ftp_config.h"
#include "lwrb.h"
#include "FreeRTOS.h"
#include "semphr.h"

#define FTP_CWD_SIZE				_MAX_LFN + 8
#define FTP_BUF_SIZE_MIN 			1024
#define FTP_BUF_SIZE 				(FTP_BUF_SIZE_MIN * FTP_BUF_SIZE_MULT)
#define FTP_USER_NAME_OK(name)		(!strcmp(name, ftp_user_name))
#define FTP_USER_PASS_OK(pass)		(!strcmp(pass, ftp_user_pass))
#define FTP_IS_LOGGED_IN(p_ftp)		(p_ftp->user == FTP_USER_USER_LOGGED_IN)
#define PORT_INCREMENT_OFFSET		25 // used for a bugfix which works around ports which are already in use (from a previous connection)
#define FTP_DATE_STRING_SIZE		64

typedef enum {
	DCM_NOT_SET,
	DCM_PASSIVE,
	DCM_ACTIVE
} dcm_type;

typedef enum {
	FTP_USER_NONE,
	FTP_USER_USER_NO_PASS,
	FTP_USER_USER_LOGGED_IN
} ftp_user_t;

typedef struct {
	uint16_t data_port;
	uint8_t data_port_incremented;
	FIL file;
	FILINFO finfo;
	char path_rename[FTP_CWD_SIZE];
	char path[FTP_CWD_SIZE];
	ALIGN_32BYTES(char ftp_buff[FTP_BUF_SIZE + 1]);
	lwrb_t lwrb;
	char date_str[FTP_DATE_STRING_SIZE];
	uint8_t ftp_con_num;
	ftp_user_t user;
	dcm_type data_conn_mode;
} ftp_data_t;

static char ftp_user_name[FTP_USER_NAME_LEN + 1] = FTP_USER_NAME_DEFAULT;
static char ftp_user_pass[FTP_USER_PASS_LEN + 1] = FTP_USER_PASS_DEFAULT;
FTP_STRUCT_MEM_SECTION(static ftp_data_t ftp_data[FTP_NBR_CLIENTS]) = {0};
static SemaphoreHandle_t data_sent_sem = { 0 };

void ftp_data_clear(uint8_t index) {
	memset(&(ftp_data[index]), 0, sizeof(ftp_data_t));
}

void ftp_data_init(uint8_t index) {
	ftp_data_clear(index);
	data_sent_sem = xSemaphoreCreateBinary();
	assert_param(data_sent_sem != NULL);
}

err_t ftp_data_sent(uint8_t index, struct tcp_pcb *tpcb, uint16_t len) {
	lwrb_skip(&(ftp_data[index].lwrb), len);
	xSemaphoreGive(data_sent_sem);
	return (ERR_OK);
}
