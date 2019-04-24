#ifndef _VIEWER_SESSION_H
#define _VIEWER_SESSION_H

/*
 * Copyright (C) 2013 Julien Desfossez <jdesfossez@efficios.com>
 * Copyright (C) 2013 David Goulet <dgoulet@efficios.com>
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <limits.h>
#include <inttypes.h>
#include <pthread.h>
#include <urcu/list.h>
#include <urcu/ref.h>

#include <common/hashtable/hashtable.h>

#include "session.h"

struct relay_viewer_session {
	/*
	 * Session list. Updates are protected by the session_list_lock.
	 * Traversals are protected by RCU.
	 * This list limits the design to having the sessions in at most
	 * one viewer session.
	 */
	struct cds_list_head session_list;	/* RCU list. */
	pthread_mutex_t session_list_lock;	/* Protects list updates. */
};

struct relay_viewer_session *viewer_session_create(void);
void viewer_session_destroy(struct relay_viewer_session *vsession);
void viewer_session_close(struct relay_viewer_session *vsession);

int viewer_session_attach(struct relay_viewer_session *vsession,
		struct relay_session *session);
int viewer_session_is_attached(struct relay_viewer_session *vsession,
		struct relay_session *session);
void viewer_session_close_one_session(struct relay_viewer_session *vsession,
		struct relay_session *session);

#endif /* _VIEWER_SESSION_H */
