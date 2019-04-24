/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2013 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef SESSIOND_KERNEL_MANAGEMENT_THREAD_H
#define SESSIOND_KERNEL_MANAGEMENT_THREAD_H

#include <stdbool.h>
#include "lttng-sessiond.h"

bool launch_kernel_management_thread(int kernel_poll_pipe_read_fd);

#endif /* SESSIOND_KERNEL_MANAGEMENT_THREAD_H */
