#ifndef RELAYD_TCP_KEEP_ALIVE_H
#define RELAYD_TCP_KEEP_ALIVE_H

/*
 * Copyright (C) 2017 Jonathan Rajotte <jonathan.rajotte-julien@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <common/macros.h>

LTTNG_HIDDEN
int socket_apply_keep_alive_config(int socket_fd);

#endif /* RELAYD_TCP_KEEP_ALIVE_H */
