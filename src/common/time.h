/*
 * Copyright (C) 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#ifndef LTTNG_TIME_H
#define LTTNG_TIME_H

#include <time.h>
#include <stdbool.h>
#include <common/macros.h>

#define MSEC_PER_SEC    1000ULL
#define NSEC_PER_SEC    1000000000ULL
#define NSEC_PER_MSEC   1000000ULL
#define NSEC_PER_USEC   1000ULL
#define USEC_PER_SEC    1000000ULL
#define USEC_PER_MSEC   1000ULL

#define SEC_PER_MINUTE  60ULL
#define MINUTE_PER_HOUR 60ULL

#define USEC_PER_MINUTE (USEC_PER_SEC * SEC_PER_MINUTE)
#define USEC_PER_HOURS  (USEC_PER_MINUTE * MINUTE_PER_HOUR)

LTTNG_HIDDEN
bool locale_supports_utf8(void);

#define NSEC_UNIT       "ns"
#define USEC_UNIT       (locale_supports_utf8() ? "µs" : "us")
#define MSEC_UNIT       "ms"
#define SEC_UNIT        "s"
#define MIN_UNIT        "m"
#define HR_UNIT         "h"

/*
 * timespec_to_ms: Convert timespec to milliseconds.
 *
 * Returns 0 on success, else -1 on error. errno is set to EOVERFLOW if
 * input would overflow the output in milliseconds.
 */
LTTNG_HIDDEN
int timespec_to_ms(struct timespec ts, unsigned long *ms);

/*
 * timespec_abs_diff: Absolute difference between timespec.
 */
LTTNG_HIDDEN
struct timespec timespec_abs_diff(struct timespec ts_a, struct timespec ts_b);

#endif /* LTTNG_TIME_H */
