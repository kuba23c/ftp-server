/*
 * ftp.h
 *
 *  Created on: Oct 8, 2025
 *      Author: jakubczekaj
 */

#ifndef FTP_SERVER_FTP_H_
#define FTP_SERVER_FTP_H_

#include <stdbool.h>

bool ftp_start(void);
bool ftp_stop(void);
void ftp_init(void);

#endif /* FTP_SERVER_FTP_H_ */
