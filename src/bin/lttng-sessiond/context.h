/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTT_CONTEXT_H
#define _LTT_CONTEXT_H

#include <lttng/lttng.h>

#include "trace-kernel.h"
#include "trace-ust.h"
#include "ust-ctl.h"

int context_kernel_add(struct ltt_kernel_session *ksession,
		struct lttng_event_context *ctx, char *channel_name);
int context_ust_add(struct ltt_ust_session *usess,
		enum lttng_domain_type domain, struct lttng_event_context *ctx,
		char *channel_name);

#endif /* _LTT_CONTEXT_H */
