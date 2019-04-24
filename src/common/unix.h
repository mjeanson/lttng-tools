/*
 * Copyright (C) 2012 David Goulet <dgoulet@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTTCOMM_UNIX_H
#define _LTTCOMM_UNIX_H

#include <limits.h>
#include <sys/un.h>

#include <common/compat/socket.h>
#include <common/macros.h>

LTTNG_HIDDEN
int lttcomm_create_unix_sock(const char *pathname);
LTTNG_HIDDEN
int lttcomm_create_anon_unix_socketpair(int *fds);
LTTNG_HIDDEN
int lttcomm_connect_unix_sock(const char *pathname);
LTTNG_HIDDEN
int lttcomm_accept_unix_sock(int sock);
LTTNG_HIDDEN
int lttcomm_listen_unix_sock(int sock);
LTTNG_HIDDEN
int lttcomm_close_unix_sock(int sock);

/* Send a message accompanied by fd(s) over a unix socket. */
LTTNG_HIDDEN
ssize_t lttcomm_send_fds_unix_sock(int sock, const int *fds, size_t nb_fd);
/* Recv a message accompanied by fd(s) from a unix socket */
LTTNG_HIDDEN
ssize_t lttcomm_recv_fds_unix_sock(int sock, int *fds, size_t nb_fd);

LTTNG_HIDDEN
ssize_t lttcomm_recv_unix_sock(int sock, void *buf, size_t len);
LTTNG_HIDDEN
ssize_t lttcomm_recv_unix_sock_non_block(int sock, void *buf, size_t len);
LTTNG_HIDDEN
ssize_t lttcomm_send_unix_sock(int sock, const void *buf, size_t len);
LTTNG_HIDDEN
ssize_t lttcomm_send_unix_sock_non_block(int sock, const void *buf, size_t len);

LTTNG_HIDDEN
ssize_t lttcomm_send_creds_unix_sock(int sock, void *buf, size_t len);
LTTNG_HIDDEN
ssize_t lttcomm_recv_creds_unix_sock(int sock, void *buf, size_t len,
		lttng_sock_cred *creds);

LTTNG_HIDDEN
int lttcomm_setsockopt_creds_unix_sock(int sock);

#endif	/* _LTTCOMM_UNIX_H */
