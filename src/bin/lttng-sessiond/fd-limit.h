#ifndef _LTTNG_FD_LIMIT_H
#define _LTTNG_FD_LIMIT_H

/*
 * Copyright (C) 2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

enum lttng_fd_type {
	LTTNG_FD_APPS,
	LTTNG_FD_NR_TYPES,
};

int lttng_fd_get(enum lttng_fd_type type, unsigned int nr);
void lttng_fd_put(enum lttng_fd_type type, unsigned int nr);
void lttng_fd_init(void);

#endif /* _LTTNG_FD_LIMIT_H */
