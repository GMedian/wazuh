/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#ifdef WIN32
#include "os_execd/execd.h"
#endif
#include "os_crypto/md5/md5_op.h"
#include "os_net/os_net.h"
#include "agentd.h"

#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/endian.h>
#elif defined(__MACH__)
#include <machine/endian.h>
#endif

/* Global variables */
static FILE *fp = NULL;
static char file_sum[34] = "";
static char file[OS_SIZE_1024 + 1] = "";


/* Receive events from the server */
int receive_msg()
{
    ssize_t recv_b;
    uint32_t length;
    int reads = 0;
    char buffer[OS_MAXSTR + 1];
    char cleartext[OS_MAXSTR + 1];
    char *tmp_msg;

    memset(cleartext, '\0', OS_MAXSTR + 1);
    memset(buffer, '\0', OS_MAXSTR + 1);

    /* Read until no more messages are available */
    while (1) {
        if (agt->protocol == TCP_PROTO) {
            /* Only one read per call */
            if (reads++) {
                break;
            }

            recv_b = recv(agt->sock, (char*)&length, sizeof(length), MSG_WAITALL);
            length = wnet_order(length);

            // Manager disconnected or error

            if (recv_b <= 0 || length > OS_MAXSTR) {
                switch (recv_b) {
                case -1:
                    if (errno == ENOTCONN) {
                        mdebug1("Manager disconnected (ENOTCONN).");
                    } else {
                        merror("Connection socket: %s (%d)", strerror(errno), errno);
                    }

                    return -1;

                case 0:
                    mdebug1("Manager disconnected.");
                    return -1;

                default:
                    // length > OS_MAXSTR
                    merror("Too big message size from manager.");
                }

                break;
            }

            recv_b = recv(agt->sock, buffer, length, MSG_WAITALL);

            if (recv_b != (ssize_t)length) {
                merror("Incorrect message size from manager: expecting %u, got %d", length, (int)recv_b);
                break;
            }
        } else {
            recv_b = recv(agt->sock, buffer, OS_SIZE_1024, MSG_DONTWAIT);

            if (recv_b <= 0) {
                break;
            }
        }

        buffer[recv_b] = '\0';

        tmp_msg = ReadSecMSG(&keys, buffer, cleartext, 0, recv_b - 1, agt->rip[agt->rip_id]);
        if (tmp_msg == NULL) {
            mwarn(MSG_ERROR, agt->rip[agt->rip_id]);
            continue;
        }

        /* Check for commands */
        if (IsValidHeader(tmp_msg)) {
            available_server = (int)time(NULL);

#ifdef WIN32
            /* Run timeout commands */
            if (agt->execdq >= 0) {
                WinTimeoutRun(available_server);
            }
#endif

            /* If it is an active response message */
            if (strncmp(tmp_msg, EXECD_HEADER, strlen(EXECD_HEADER)) == 0) {
                tmp_msg += strlen(EXECD_HEADER);
#ifndef WIN32
                if (agt->execdq >= 0) {
                    if (OS_SendUnix(agt->execdq, tmp_msg, 0) < 0) {
                        merror("Error communicating with execd");
                    }
                }
#else
                /* Run on Windows */
                if (agt->execdq >= 0) {
                    WinExecdRun(tmp_msg);
                }
#endif

                continue;
            }

            /* Restart syscheck */
            else if (strcmp(tmp_msg, HC_SK_RESTART) == 0) {
                os_set_restart_syscheck();
                continue;
            }

            /* Ack from server */
            else if (strcmp(tmp_msg, HC_ACK) == 0) {
                continue;
            }

            /* Close any open file pointer if it was being written to */
            if (fp) {
                fclose(fp);
                fp = NULL;
            }

            /* File update message */
            if (strncmp(tmp_msg, FILE_UPDATE_HEADER,
                        strlen(FILE_UPDATE_HEADER)) == 0) {
                char *validate_file;

                tmp_msg += strlen(FILE_UPDATE_HEADER);

                /* Going to after the file sum */
                validate_file = strchr(tmp_msg, ' ');
                if (!validate_file) {
                    continue;
                }

                *validate_file = '\0';

                /* Copy the file sum */
                strncpy(file_sum, tmp_msg, 33);

                /* Set tmp_msg to the beginning of the file name */
                validate_file++;
                tmp_msg = validate_file;

                if ((validate_file = strchr(tmp_msg, '\n')) != NULL) {
                    *validate_file = '\0';
                }

                while ((validate_file = strchr(tmp_msg, '/')) != NULL) {
                    *validate_file = '-';
                }

                if (tmp_msg[0] == '.') {
                    tmp_msg[0] = '-';
                }

                snprintf(file, OS_SIZE_1024, "%s/%s",
                         SHAREDCFG_DIR,
                         tmp_msg);

                fp = fopen(file, "w");
                if (!fp) {
                    merror(FOPEN_ERROR, file, errno, strerror(errno));
                }
            }

            else if (strncmp(tmp_msg, FILE_CLOSE_HEADER,
                             strlen(FILE_CLOSE_HEADER)) == 0) {
                /* No error */
                os_md5 currently_md5;

                /* Close for the rename to work */
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }

                if (file[0] == '\0') {
                    /* Nothing to be done */
                }

                else if (OS_MD5_File(file, currently_md5, OS_TEXT) < 0) {
                    /* Remove file */
                    unlink(file);
                    file[0] = '\0';
                } else {
                    if (strcmp(currently_md5, file_sum) != 0) {
                        mdebug1("Failed md5 for: %s -- deleting.",
                               file);
                        unlink(file);
                    } else {
                        char *final_file;

                        /* Rename the file to its original name */
                        final_file = strrchr(file, '/');
                        if (final_file) {
                            if (strcmp(final_file + 1, SHAREDCFG_FILENAME) == 0) {
                                UnmergeFiles(file, SHAREDCFG_DIR);
                            }
                        } else {
                            /* Remove file */
                            unlink(file);
                        }
                    }

                    file[0] = '\0';
                }
            }

            else {
                mwarn("Unknown message received from server.");
            }
        }

        else if (fp) {
            available_server = (int)time(NULL);
            fprintf(fp, "%s", tmp_msg);
        }

        else {
            mwarn("Unknown message received. No action defined.");
        }
    }

    return 0;
}
