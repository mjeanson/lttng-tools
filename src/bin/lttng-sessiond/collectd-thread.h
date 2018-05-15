/*
 * Copyright (C) 2018 Michael Jeanson <mjeanson@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 only, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef COLLECTD_THREAD_H
#define COLLECTD_THREAD_H

#include <semaphore.h>

struct collectd_thread_handle {
	/* quit pipe */
	int thread_quit_pipe;

	/*
	 * To wait for the app registration thread.
	 */
	sem_t *registration_thread_ready;
};

void collectd_thread_handle_destroy(struct collectd_thread_handle *handle);
struct collectd_thread_handle *collectd_thread_handle_create(
		int thread_quit_pipe,
		sem_t *registration_thread_ready);

#ifdef HAVE_LIBLTTNG_UST_CTL

void *thread_manage_collectd(void *data);

#else /* HAVE_LIBLTTNG_UST_CTL */

void *thread_manage_collectd(void *data);
{
	return NULL;
}

#endif /* HAVE_LIBLTTNG_UST_CTL */

#endif /* COLLECTD_THREAD_H */
