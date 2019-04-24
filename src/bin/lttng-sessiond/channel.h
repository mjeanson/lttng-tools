/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTT_CHANNEL_H
#define _LTT_CHANNEL_H

#include <lttng/lttng.h>

#include "trace-kernel.h"
#include "trace-ust.h"

int channel_kernel_disable(struct ltt_kernel_session *ksession,
		char *channel_name);
int channel_kernel_enable(struct ltt_kernel_session *ksession,
		struct ltt_kernel_channel *kchan);
int channel_kernel_create(struct ltt_kernel_session *ksession,
		struct lttng_channel *chan, int kernel_pipe);

struct lttng_channel *channel_new_default_attr(int domain,
		enum lttng_buffer_type type);
void channel_attr_destroy(struct lttng_channel *channel);

int channel_ust_create(struct ltt_ust_session *usess,
		struct lttng_channel *attr, enum lttng_buffer_type type);
int channel_ust_enable(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan);
int channel_ust_disable(struct ltt_ust_session *usess,
		struct ltt_ust_channel *uchan);

#endif /* _LTT_CHANNEL_H */
