/*
 * Copyright (C) 2018 Jonathan Rajotte <jonathan.rajotte-julien@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef RELAYD_CMD_2_11_H
#define RELAYD_CMD_2_11_H

#include "lttng-relayd.h"
#include <common/buffer-view.h>

int cmd_create_session_2_11(const struct lttng_buffer_view *payload,
		char *session_name, char *hostname,
		uint32_t *live_timer, bool *snapshot);

int cmd_recv_stream_2_11(const struct lttng_buffer_view *payload,
		char **ret_path_name, char **ret_channel_name,
		uint64_t *tracefile_size, uint64_t *tracefile_count,
		uint64_t *trace_archive_id);

#endif /* RELAYD_CMD_2_11_H */
