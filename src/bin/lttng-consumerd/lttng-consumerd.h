/*
 * Copyright (C) 2011 Julien Desfossez <julien.desfossez@polymtl.ca>
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTTNG_CONSUMERD_H
#define _LTTNG_CONSUMERD_H


#define NR_LTTNG_CONSUMER_READY		1
extern int lttng_consumer_ready;

const char *tracing_group_name;

enum lttng_consumer_type lttng_consumer_get_type(void);

#endif /* _LTTNG_CONSUMERD_H */
