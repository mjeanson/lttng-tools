/*
 * Copyright (C) 2011 David Goulet <david.goulet@polymtl.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTT_UTILS_H
#define _LTT_UTILS_H

struct lttng_ht;
struct ltt_session;

const char *get_home_dir(void);
int notify_thread_pipe(int wpipe);
void ht_cleanup_push(struct lttng_ht *ht);
int loglevels_match(int a_loglevel_type, int a_loglevel_value,
	int b_loglevel_type, int b_loglevel_value, int loglevel_all_type);
const char *session_get_base_path(const struct ltt_session *session);

#endif /* _LTT_UTILS_H */
