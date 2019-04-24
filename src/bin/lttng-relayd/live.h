#ifndef LTTNG_RELAYD_LIVE_H
#define LTTNG_RELAYD_LIVE_H

/*
 * Copyright (C) 2013 Julien Desfossez <jdesfossez@efficios.com>
 * Copyright (C) 2013 David Goulet <dgoulet@efficios.com>
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <common/uri.h>

#include "lttng-relayd.h"

int relayd_live_create(struct lttng_uri *live_uri);
int relayd_live_stop(void);
int relayd_live_join(void);

struct relay_viewer_stream *live_find_viewer_stream_by_id(uint64_t stream_id);

#endif /* LTTNG_RELAYD_LIVE_H */
