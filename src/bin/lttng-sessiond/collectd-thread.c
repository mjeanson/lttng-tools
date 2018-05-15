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

#define _LGPL_SOURCE
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <common/defaults.h>
#include <common/common.h>

#include "lttng-sessiond.h"
#include "collectd-thread.h"

void collectd_thread_handle_destroy(
		struct collectd_thread_handle *handle)
{
	if (!handle) {
		goto end;
	}

end:
	free(handle);
}

struct collectd_thread_handle *collectd_thread_handle_create(
		int thread_quit_pipe,
		sem_t *registration_thread_ready)
{
	struct collectd_thread_handle *handle;

	handle = zmalloc(sizeof(*handle));
	if (!handle) {
		goto end;
	}

	handle->thread_quit_pipe = thread_quit_pipe;
	handle->registration_thread_ready = registration_thread_ready;
end:
	return handle;
}

/*
 * Returns
 *  -1 : On error.
 *   0 : On disconnection (EOF).
 *   1 : On success.
 */
static int _read_collectd_magic_byte(int fd)
{
	int ret;
	unsigned char buffer[1];

	if (fd < 1) {
		ret = -1;
		goto end;
	}

restart:
	ret = read(fd, &buffer, 1);
	if (ret == -1) {
		if (errno == EINTR || errno == EAGAIN) {
			goto restart;
		} else {
			goto end;
		}
	}
	if (ret == 0) {
		/* EOF */
		goto end;
	}
	if (buffer[0] != DEFAULT_LTTNG_COLLECTD_MAGIC) {
		DBG("[collectd-thread] Received invalid byte %x", buffer[0]);
		ret = -1;
	}
end:
	return ret;
}

/*
 * This thread manage the collect daemon.
 */
void *thread_manage_collectd(void *data)
{
	int ret, fd_r = 0;
	struct collectd_thread_handle *handle = data;
	char *bin_path = config.collectd_bin_path.value;
	char *pipe_path = config.collectd_pipe_path.value;

	DBG("[collectd-thread] Manage collectd thread started");

	if (!handle) {
		ERR("[collectd-thread] Invalid thread context provided");
		goto error;
	}

	/*
	 * We wait until the app registration thread is ready.
	 */
	sem_wait(handle->registration_thread_ready);


	// creates/open a fifo in the rundir (see MKFIFO(3))
	DBG("[collectd-thread] Creating collectd pipe at %s", pipe_path);

	/* Create the named pipe */
	ret = mkfifo(pipe_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret) {
		PERROR("mkfifo");
		goto error;
	}

	/*
	 * Open the read side of the named pipe with O_NONBLOCK so we can
	 * continue without the other side being open for write
	 */
	fd_r = open(pipe_path, O_RDONLY | O_NONBLOCK);
	if (fd_r < 0) {
		PERROR("open fifo");
		goto error;
	}

	// fork()+execve() the lttng-collectd with the path to the fifo as argument
	ret = fork();

	if (ret == -1) {
		PERROR("fork");
		goto error;
	}
	if (ret == 0) {
		/*
		 * The child will execve the lttng-collectd binary with the path
		 * to the named pipe as an argument and pass the current env with
		 * the addition of LTTNG_UST_REGISTER_TIMEOUT set to -1 so that it
		 * waits indefinatly for registration with the sessiond.
		 */
		extern char** environ;
		char *newargv[] = {"lttng-collectd", pipe_path, NULL};

		ret = setenv("LTTNG_UST_REGISTER_TIMEOUT", "-1", 1);
		if (ret == -1) {
			PERROR("setenv");
			goto error_child;
		}

		DBG("[collectd-thread] Execve %s", bin_path);
		ret = execve(bin_path, newargv, environ);
		if (ret == -1) {
			PERROR("execve");
		}
error_child:
		return NULL;
	} else {
		/* Parent */
		int i, pollfd;
		uint32_t revents, nb_fd;
		int timeout = DEFAULT_LTTNG_COLLECTD_WAIT_TIMEOUT;
		struct lttng_poll_event events;

		/*
		 * Create pollset with size 2:
		 *   - sessiond quit pipe
		 *   - collectd named pipe
		 */
		ret = lttng_poll_create(&events, 2, LTTNG_CLOEXEC);
		if (ret < 0) {
			goto error_parent;
		}

		DBG("[collectd-thread] Adding thread_quit_pipe fd (%i) to pollset", handle->thread_quit_pipe);
		ret = lttng_poll_add(&events, handle->thread_quit_pipe, LPOLLIN);
		if (ret < 0) {
			ERR("[collectd-thread] Failed to add thread_quit_pipe fd to pollset");
			goto error_parent;
		}

		DBG("[collectd-thread] Adding collectd pipe fd (%i) to pollset", fd_r);
		ret = lttng_poll_add(&events, fd_r, LPOLLIN);
		if (ret < 0) {
			ERR("[collectd-thread] Failed to add collectd pipe fd to pollset");
			goto error_parent;
		}

restart_poll:
		DBG("[collectd-thread] Entering poll wait");
		ret = lttng_poll_wait(&events, timeout);
		DBG("[collectd-thread] Poll wait returned (%i)", ret);

		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart_poll;
			}
			ERR("[collectd-thread] Error encountered during lttng_poll_wait (%i)", ret);
			goto error_parent;
		}
		if (ret == 0) {
			ERR("[collectd-thread] Timeout waiting for lttng-collectd");
			goto error_parent;
		}

		/*
		 * We have events to handle either on the close pipe or the
		 * collectd named pipe.
		 */
		nb_fd = ret;
		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			DBG("[collectd-thread] Handling fd (%i) activity (%u)", pollfd, revents);

			if (!revents) {
				/* No activity for this FD (poll implementation). */
				continue;
			}

			/* Thread quit pipe has been closed. Killing thread. */
			if (pollfd == handle->thread_quit_pipe) {
				DBG("[collectd-thread] Quit pipe activity");
				goto exit_parent;
			}

			/* Event on the collectd named pipe */
			if (pollfd == fd_r) {
				if (revents & LPOLLIN) {
					// Read the magic byte
					ret = _read_collectd_magic_byte(pollfd);
					if (ret == -1) {
						ERR("[collectd-thread] Error reading from child");
						goto error_parent;
					}
					if (ret == 0) {
						ERR("[collectd-thread] lttng-collectd child disconnected");
						goto error_parent;
					}
					if (ret == 1) {
						DBG("[collectd-thread] Connected to lttng-collectd");
						/* Now that we are connected, remove the timeout */
						timeout = -1;
					}
					continue;
				} else if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("[collectd-thread] lttng-collectd child disconnected");
					goto error_parent;
				} else {
					ERR("Unexpected poll events %u for sock %d", revents, pollfd);
					goto error_parent;
				}
			}
		}
		goto restart_poll;
exit_parent:
error_parent:
		lttng_poll_clean(&events);
	}

error:
	unlink(pipe_path);
	if (fd_r > 0) {
		do {
			ret = close(fd_r);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0) {
			PERROR("close collectd read pipe");
		}
	}

	DBG("[collectd-thread] cleanup completed");

	return NULL;
}
