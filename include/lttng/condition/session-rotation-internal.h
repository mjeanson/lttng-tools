/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_CONDITION_SESSION_ROTATION_INTERNAL_H
#define LTTNG_CONDITION_SESSION_ROTATION_INTERNAL_H

#include <lttng/condition/session-rotation.h>
#include <lttng/condition/condition-internal.h>
#include <lttng/condition/evaluation-internal.h>
#include "common/buffer-view.h"
#include <lttng/location.h>
#include <common/macros.h>

struct lttng_condition_session_rotation {
	struct lttng_condition parent;
	char *session_name;
};

struct lttng_condition_session_rotation_comm {
	/* Length includes the trailing \0. */
	uint32_t session_name_len;
	char session_name[];
} LTTNG_PACKED;

struct lttng_evaluation_session_rotation {
	struct lttng_evaluation parent;
	uint64_t id;
	struct lttng_trace_archive_location *location;
};

struct lttng_evaluation_session_rotation_comm {
	uint64_t id;
	uint8_t has_location;
} LTTNG_PACKED;

LTTNG_HIDDEN
ssize_t lttng_condition_session_rotation_ongoing_create_from_buffer(
		const struct lttng_buffer_view *view,
		struct lttng_condition **condition);

LTTNG_HIDDEN
ssize_t lttng_condition_session_rotation_completed_create_from_buffer(
		const struct lttng_buffer_view *view,
		struct lttng_condition **condition);

LTTNG_HIDDEN
struct lttng_evaluation *lttng_evaluation_session_rotation_ongoing_create(
		 uint64_t id);

/* Ownership of location is transferred to the evaluation. */
LTTNG_HIDDEN
struct lttng_evaluation *lttng_evaluation_session_rotation_completed_create(
		uint64_t id,
		struct lttng_trace_archive_location *location);

LTTNG_HIDDEN
ssize_t lttng_evaluation_session_rotation_ongoing_create_from_buffer(
		const struct lttng_buffer_view *view,
		struct lttng_evaluation **evaluation);

LTTNG_HIDDEN
ssize_t lttng_evaluation_session_rotation_completed_create_from_buffer(
		const struct lttng_buffer_view *view,
		struct lttng_evaluation **evaluation);

#endif /* LTTNG_CONDITION_SESSION_ROTATION_INTERNAL_H */
