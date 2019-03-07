#ifndef LTTNG_RELAYD_H
#define LTTNG_RELAYD_H

/*
 * Copyright (C) 2012 Julien Desfossez <jdesfossez@efficios.com>
 * Copyright (C) 2012 David Goulet <dgoulet@efficios.com>
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <limits.h>
#include <urcu.h>
#include <urcu/wfcqueue.h>

#include <common/hashtable/hashtable.h>

/*
 * Queue used to enqueue relay requests
 */
struct relay_conn_queue {
	struct cds_wfcq_head head;
	struct cds_wfcq_tail tail;
	int32_t futex;
};

/*
 * Contains stream indexed by ID. This is important since many commands lookup
 * streams only by ID thus also keeping them in this hash table makes the
 * search O(1).
 */
extern struct lttng_ht *sessions_ht;
extern struct lttng_ht *relay_streams_ht;
extern struct lttng_ht *viewer_streams_ht;

extern char *opt_output_path;
extern const char *tracing_group_name;
extern const char * const config_section_name;

extern int thread_quit_pipe[2];

void lttng_relay_notify_ready(void);
int lttng_relay_stop_threads(void);

#endif /* LTTNG_RELAYD_H */
