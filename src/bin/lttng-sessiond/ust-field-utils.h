/*
 * Copyright (C) 2018 Francis Deslauriers francis.deslauriers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_UST_FIELD_UTILS_H
#define LTTNG_UST_FIELD_UTILS_H

#include "ust-ctl.h"

/*
 * Compare two UST fields.
 * Return 1 if both fields have identical definition, 0 otherwise.
 */
int match_ustctl_field(const struct ustctl_field *first,
		const struct ustctl_field *second);

#endif /* LTTNG_UST_FIELD_UTILS_H */
