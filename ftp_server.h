/*
 * ftp_server.h
 *
 *  Created on: Aug 31, 2020
 *      Author: Sande
 */

#ifndef _FTP_SERVER_H_
#define _FTP_SERVER_H_

/**
 * Setter functions for username and password
 */
extern void ftp_set_username(const char *name);
extern void ftp_set_password(const char *pass);

/**
 * Start the FTP server.
 *
 * This code creates a socket on port 21 to listen for incoming
 * FTP client connections. If this creation fails the code returns
 * Immediately. If the socket is created the task continues.
 *
 * The task loops indefinitely and waits for connections. When a
 * connection is found a port is assigned to the incoming client.
 * A separate task is started for each connection which handles
 * The FTP commands. When the client disconnects the task is
 * stopped.
 *
 * An incoming connection is denied when:
 * - The memory on the CMS is not available
 * - The maximum number of clients is connected
 * - The application is running
 */
void ftp_server(void *argument);

#endif /* ETH_FTP_FTP_SERVER_H_ */
