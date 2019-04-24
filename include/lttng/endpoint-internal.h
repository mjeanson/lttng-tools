/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_ENDPOINT_INTERNAL_H
#define LTTNG_ENDPOINT_INTERNAL_H

#include <lttng/endpoint.h>
#include <common/macros.h>

enum lttng_endpoint_type {
	LTTNG_ENDPOINT_TYPE_DEFAULT_SESSIOND_NOTIFICATION = 0,
};

struct lttng_endpoint {
	enum lttng_endpoint_type type;
};

#endif /* LTTNG_ENDPOINT_INTERNAL_H */
