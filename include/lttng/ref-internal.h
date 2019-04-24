#ifndef LTTNG_REF_INTERNAL_H
#define LTTNG_REF_INTERNAL_H

/*
 * LTTng - Non thread-safe reference counting
 *
 * Copyright 2013, 2014 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <assert.h>

typedef void (*lttng_release_func)(void *);

struct lttng_ref {
	unsigned long count;
	lttng_release_func release;
};

static inline
void lttng_ref_init(struct lttng_ref *ref, lttng_release_func release)
{
	assert(ref);
	ref->count = 1;
	ref->release = release;
}

static inline
void lttng_ref_get(struct lttng_ref *ref)
{
	assert(ref);
	ref->count++;
	/* Overflow check. */
	assert(ref->count);
}

static inline
void lttng_ref_put(struct lttng_ref *ref)
{
	assert(ref);
	/* Underflow check. */
	assert(ref->count);
	if (caa_unlikely((--ref->count) == 0)) {
		ref->release(ref);
	}
}

#endif /* LTTNG_REF_INTERNAL_H */
