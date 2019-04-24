/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <lttng/endpoint-internal.h>

static
struct lttng_endpoint lttng_session_daemon_notification_endpoint_instance = {
	.type = LTTNG_ENDPOINT_TYPE_DEFAULT_SESSIOND_NOTIFICATION
};

struct lttng_endpoint *lttng_session_daemon_notification_endpoint =
		&lttng_session_daemon_notification_endpoint_instance;
