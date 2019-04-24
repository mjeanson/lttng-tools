/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright (C) 2013 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef CLIENT_SESSIOND_H
#define CLIENT_SESSIOND_H

#include "thread.h"

struct lttng_thread *launch_client_thread(void);

#endif /* CLIENT_SESSIOND_H */
