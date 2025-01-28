# LWIP-FreeRTOS-Netconn-FTP-Server
 FTP Server for Embedded devices created uder FreeRTOS with LWIP Netconn API.

 Started working from: https://github.com/gallegojm/STM32-E407-FtpServer
 Forked from: https://github.com/sandertrilectronics/LWIP-FreeRTOS-Netconn-FTP-Server.git

 What changed:
 - Fixed some bugs,
 - Cleanup code a litte,
 - Added all most important options to one file `ftp_config.h`,
 - Now this library can be easily customized with file `ftp_custom.h`,

# How to use this library
- include `ftp_server.h` to your project
- create `ftp_custom.h` file, in which you can overwrite options from `ftp_config.h`
- create task for `ftp_server` function (start this task after lwip and fatfs initialization)
