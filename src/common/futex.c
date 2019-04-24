/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include <limits.h>
#include <unistd.h>
#include <urcu.h>
#include <urcu/futex.h>

#include <common/common.h>

#include "futex.h"

/*
 * This futex wait/wake scheme only works for N wakers / 1 waiters. Hence the
 * "nto1" added to all function signature.
 *
 * Please see wait_gp()/update_counter_and_wait() calls in urcu.c in the urcu
 * git tree for a detail example of this scheme being used. futex_async() is
 * the urcu wrapper over the futex() sycall.
 *
 * There is also a formal verification available in the git tree.
 *
 *   branch: formal-model
 *   commit id: 2a8044f3493046fcc8c67016902dc7beec6f026a
 *
 * Ref: git://git.lttng.org/userspace-rcu.git
 */

/*
 * Update futex according to active or not. This scheme is used to wake every
 * libust waiting on the shared memory map futex hence the INT_MAX used in the
 * futex() call. If active, we set the value and wake everyone else we indicate
 * that we are gone (cleanup() case).
 */
LTTNG_HIDDEN
void futex_wait_update(int32_t *futex, int active)
{
	if (active) {
		uatomic_set(futex, 1);
		if (futex_async(futex, FUTEX_WAKE,
				INT_MAX, NULL, NULL, 0) < 0) {
			PERROR("futex_async");
			abort();
		}
	} else {
		uatomic_set(futex, 0);
	}

	DBG("Futex wait update active %d", active);
}

/*
 * Prepare futex.
 */
LTTNG_HIDDEN
void futex_nto1_prepare(int32_t *futex)
{
	uatomic_set(futex, -1);
	cmm_smp_mb();

	DBG("Futex n to 1 prepare done");
}

/*
 * Wait futex.
 */
LTTNG_HIDDEN
void futex_nto1_wait(int32_t *futex)
{
	cmm_smp_mb();

	if (uatomic_read(futex) != -1)
		goto end;
	while (futex_async(futex, FUTEX_WAIT, -1, NULL, NULL, 0)) {
		switch (errno) {
		case EWOULDBLOCK:
			/* Value already changed. */
			goto end;
		case EINTR:
			/* Retry if interrupted by signal. */
			break;	/* Get out of switch. */
		default:
			/* Unexpected error. */
			PERROR("futex_async");
			abort();
		}
	}
end:
	DBG("Futex n to 1 wait done");
}

/*
 * Wake 1 futex.
 */
LTTNG_HIDDEN
void futex_nto1_wake(int32_t *futex)
{
	if (caa_unlikely(uatomic_read(futex) != -1))
		goto end;
	uatomic_set(futex, 0);
	if (futex_async(futex, FUTEX_WAKE, 1, NULL, NULL, 0) < 0) {
		PERROR("futex_async");
		abort();
	}
end:
	DBG("Futex n to 1 wake done");
}
