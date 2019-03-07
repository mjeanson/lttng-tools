/*
 * ust-ctl.h
 *
 * Meta header used to include all relevant file from the lttng ust ABI.
 *
 * Copyright (C) 2013 David Goulet <dgoulet@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_UST_CTL_H
#define LTTNG_UST_CTL_H

struct ust_app;

/*
 * FIXME: temporary workaround: we use a lttng-tools local version of
 * lttng-ust-abi.h if UST is not found. Eventually, we should use our
 * own internal structures within lttng-tools instead of relying on the
 * UST ABI.
 */
#ifdef HAVE_LIBLTTNG_UST_CTL

#include <lttng/ust-ctl.h>
#include <lttng/ust-abi.h>
#include <lttng/ust-error.h>

int ust_app_release_object(struct ust_app *app, struct lttng_ust_object_data *data);

#else /* HAVE_LIBLTTNG_UST_CTL */

#include "lttng-ust-ctl.h"
#include "lttng-ust-abi.h"
#include "lttng-ust-error.h"

static inline
int ust_app_release_object(struct ust_app *app, struct lttng_ust_object_data *data)
{
	return 0;
}

#endif /* HAVE_LIBLTTNG_UST_CTL */

#endif /* _LTT_UST_CTL_H */
