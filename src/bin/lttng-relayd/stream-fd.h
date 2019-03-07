#ifndef _STREAM_FD_H
#define _STREAM_FD_H

/*
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <urcu/ref.h>

struct stream_fd {
	int fd;
	struct urcu_ref ref;
};

struct stream_fd *stream_fd_create(int fd);
void stream_fd_get(struct stream_fd *sf);
void stream_fd_put(struct stream_fd *sf);

#endif /* _STREAM_FD_H */
