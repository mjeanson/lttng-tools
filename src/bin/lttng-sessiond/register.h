/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2013 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef SESSIOND_APPLICATION_REGISTRATION_THREAD_H
#define SESSIOND_APPLICATION_REGISTRATION_THREAD_H

#include <stdbool.h>
#include "lttng-sessiond.h"

struct lttng_thread *launch_application_registration_thread(
		struct ust_cmd_queue *cmd_queue);

#endif /* SESSIOND_APPLICATION_REGISTRATION_THREAD_H */
