/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_ACTION_H
#define LTTNG_ACTION_H

struct lttng_action;

#ifdef __cplusplus
extern "C" {
#endif

enum lttng_action_type {
	LTTNG_ACTION_TYPE_UNKNOWN = -1,
	LTTNG_ACTION_TYPE_NOTIFY = 0,
};

/*
 * Get the type of an action.
 *
 * Returns the type of an action on success, LTTNG_ACTION_TYPE_UNKNOWN on error.
 */
extern enum lttng_action_type lttng_action_get_type(
		struct lttng_action *action);

/*
 * Destroy (frees) an action object.
 */
extern void lttng_action_destroy(struct lttng_action *action);

#ifdef __cplusplus
}
#endif

#endif /* LTTNG_ACTION_H */
