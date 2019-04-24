/*
 * Copyright (C) 2013 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <common/time.h>
#include <common/macros.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <locale.h>
#include <string.h>

static bool utf8_output_supported;

LTTNG_HIDDEN
bool locale_supports_utf8(void)
{
	return utf8_output_supported;
}

LTTNG_HIDDEN
int timespec_to_ms(struct timespec ts, unsigned long *ms)
{
	unsigned long res, remain_ms;

	if (ts.tv_sec > ULONG_MAX / MSEC_PER_SEC) {
		errno = EOVERFLOW;
		return -1;	/* multiplication overflow */
	}
	res = ts.tv_sec * MSEC_PER_SEC;
	remain_ms = ULONG_MAX - res;
	if (ts.tv_nsec / NSEC_PER_MSEC > remain_ms) {
		errno = EOVERFLOW;
		return -1;	/* addition overflow */
	}
	res += ts.tv_nsec / NSEC_PER_MSEC;
	*ms = res;
	return 0;
}

LTTNG_HIDDEN
struct timespec timespec_abs_diff(struct timespec t1, struct timespec t2)
{
	uint64_t ts1 = (uint64_t) t1.tv_sec * (uint64_t) NSEC_PER_SEC +
			(uint64_t) t1.tv_nsec;
	uint64_t ts2 = (uint64_t) t2.tv_sec * (uint64_t) NSEC_PER_SEC +
			(uint64_t) t2.tv_nsec;
	uint64_t diff = max(ts1, ts2) - min(ts1, ts2);
	struct timespec res;

	res.tv_sec = diff / (uint64_t) NSEC_PER_SEC;
	res.tv_nsec = diff % (uint64_t) NSEC_PER_SEC;
	return res;
}

static
void __attribute__((constructor)) init_locale_utf8_support(void)
{
	const char *program_locale = setlocale(LC_ALL, NULL);
	const char *lang = getenv("LANG");

	if (program_locale && strstr(program_locale, "utf8")) {
		utf8_output_supported = true;
	} else if (strstr(lang, "utf8")) {
		utf8_output_supported = true;
	}
}
