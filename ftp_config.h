/*
 * ftp_config.h
 *
 *  Created on: Jan 28, 2025
 *      Author: kuba23c
 */

#ifndef FTP_SERVER_FTP_CONFIG_H_
#define FTP_SERVER_FTP_CONFIG_H_

#include "ftp_custom.h"

/* *********** DEBUG ************** */
#ifndef FTP_LOG_PRINT
#define FTP_LOG_PRINT(...) do {} while(0)
#endif

/* *********** FREERTOS TASK ************** */
#ifndef FTP_TASK_STATIC
#define FTP_TASK_STATIC	0
#endif

#ifndef FTP_TASK_STACK_SIZE
#define FTP_TASK_STACK_SIZE	1536
#endif

#ifndef FTP_TASK_PRIORITY
#define FTP_TASK_PRIORITY 24
#endif

/* *********** LWIP ************** */
#ifndef FTP_SERVER_PORT
#define FTP_SERVER_PORT 21
#endif

#ifndef FTP_DATA_PORT
#define FTP_DATA_PORT 55600
#endif

#ifndef FTP_NBR_CLIENTS
#define FTP_NBR_CLIENTS	1 //same as netbuf limit
#endif

#ifndef FTP_SERVER_READ_TIMEOUT_MS
#define FTP_SERVER_READ_TIMEOUT_MS 1000
#endif

#ifndef FTP_SERVER_INACTIVE_CNT
#define FTP_SERVER_INACTIVE_CNT 60 // Inactivity time (ms): FTP_SERVER_INACTIVE_CNT * FTP_SERVER_READ_TIMEOUT_MS
#endif

#ifndef FTP_PSV_ACCEPT_TIMEOUT_MS
#define FTP_PSV_ACCEPT_TIMEOUT_MS 500
#endif

#ifndef FTP_PSV_LISTEN_TIMEOUT_MS
#define FTP_PSV_LISTEN_TIMEOUT_MS 5000
#endif

#ifndef FTP_USE_PASSIVE_MODE
#define FTP_USE_PASSIVE_MODE 1
#endif
/* *********** USER/PASS ************** */
#ifndef FTP_USER_NAME_LEN
#define FTP_USER_NAME_LEN 32
#endif

#ifndef FTP_USER_PASS_LEN
#define FTP_USER_PASS_LEN 32
#endif

#ifndef FTP_USER_NAME_DEFAULT
#define FTP_USER_NAME_DEFAULT "user"
#endif

#ifndef FTP_USER_PASS_DEFAULT
#define FTP_USER_PASS_DEFAULT "pass"
#endif

/* *********** FATFS ************** */
#ifndef FTP_CUSTOM_FATFS
#include "fatfs.h"
#define FTP_F_STAT(path, fno) 				f_stat(path, fno)
#define FTP_F_OPENDIR(dp, path) 			f_opendir(dp, path)
#define FTP_F_READDIR(dp, fno) 				f_readdir(dp, fno)
#define FTP_F_UNLINK(path) 					f_unlink(path)
#define FTP_F_OPEN(fp, path, mode) 			f_open(fp, path, mode)
#define FTP_F_SIZE(fp) 						f_size(fp)
#define FTP_F_CLOSE(fp) 					f_close(fp)
#define FTP_F_WRITE(fp, buff, btw, bw) 		f_write(fp, buff, btw, bw)
#define FTP_F_READ(fp, buff, btr, br) 		f_read(fp, buff, btr, br)
#define FTP_F_MKDIR(path) 					f_mkdir(path)
#define FTP_F_RENAME(path_old, path_new) 	f_rename(path_old, path_new)
#define FTP_F_UTIME(path, fno) 				f_utime(path, fno)
#define FTP_F_GETFREE(path, nclst, fatfs) 	f_getfree(path, nclst, fatfs)
#endif /* FTP_CUSTOM_FATFS */

/* *********** CALLBACKS ************** */
#ifndef FTP_CONNECTED_CALLBACK
#define FTP_CONNECTED_CALLBACK() do {} while(0)
#endif

#ifndef FTP_DISCONNECTED_CALLBACK
#define FTP_DISCONNECTED_CALLBACK() do {} while(0)
#endif

#ifndef FTP_ETH_IS_LINK_UP
#define FTP_ETH_IS_LINK_UP() ((uint8_t)1)
#endif

#endif /* FTP_SERVER_FTP_CONFIG_H_ */
