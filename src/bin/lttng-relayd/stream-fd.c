/*
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <common/common.h>

#include "stream-fd.h"

struct stream_fd *stream_fd_create(int fd)
{
	struct stream_fd *sf;

	sf = zmalloc(sizeof(*sf));
	if (!sf) {
		goto end;
	}
	urcu_ref_init(&sf->ref);
	sf->fd = fd;
end:
	return sf;
}

void stream_fd_get(struct stream_fd *sf)
{
	urcu_ref_get(&sf->ref);
}

static void stream_fd_release(struct urcu_ref *ref)
{
	struct stream_fd *sf = caa_container_of(ref, struct stream_fd, ref);
	int ret;

	ret = close(sf->fd);
	if (ret) {
		PERROR("Error closing stream FD %d", sf->fd);
	}
	free(sf);
}

void stream_fd_put(struct stream_fd *sf)
{
	urcu_ref_put(&sf->ref, stream_fd_release);
}
