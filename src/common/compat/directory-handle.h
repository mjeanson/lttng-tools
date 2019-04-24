/*
 * Copyright (C) 2019 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _COMPAT_DIRECTORY_HANDLE_H
#define _COMPAT_DIRECTORY_HANDLE_H

#include <common/macros.h>
#include <common/credentials.h>

/*
 * Some platforms, such as Solaris 10, do not support directory file descriptors
 * and their associated functions (*at(...)), which are defined in POSIX.2008.
 *
 * This wrapper provides a handle that is either a copy of a directory's path
 * or a directory file descriptors, depending on the platform's capabilities.
 */
#ifdef COMPAT_DIRFD
struct lttng_directory_handle {
	int dirfd;
};
#else
struct lttng_directory_handle {
	char *base_path;
};
#endif

/*
 * Initialize a directory handle to the provided path. Passing a NULL path
 * returns a handle to the current working directory. The working directory
 * is not sampled; it will be accessed at the time of use of the functions
 * of this API.
 *
 * An initialized directory handle must be finalized using
 * lttng_directory_handle_fini().
 */
LTTNG_HIDDEN
int lttng_directory_handle_init(struct lttng_directory_handle *handle,
		const char *path);

LTTNG_HIDDEN
int lttng_directory_handle_init_from_dirfd(
		struct lttng_directory_handle *handle, int dirfd);

/*
 * Release the resources of a directory handle.
 */
LTTNG_HIDDEN
void lttng_directory_handle_fini(struct lttng_directory_handle *handle);

/*
 * Create a subdirectory relative to a directory handle.
 */
LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory(
		const struct lttng_directory_handle *handle,
		const char *subdirectory,
		mode_t mode);

/*
 * Create a subdirectory relative to a directory handle
 * as a given user.
 */
LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_as_user(
		const struct lttng_directory_handle *handle,
		const char *subdirectory,
		mode_t mode, struct lttng_credentials *creds);

/*
 * Recursively create a directory relative to a directory handle.
 */
LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_recursive(
		const struct lttng_directory_handle *handle,
		const char *subdirectory_path,
		mode_t mode);

/*
 * Recursively create a directory relative to a directory handle
 * as a given user.
 */
LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_recursive_as_user(
		const struct lttng_directory_handle *handle,
		const char *subdirectory_path,
		mode_t mode, struct lttng_credentials *creds);

#endif /* _COMPAT_PATH_HANDLE_H */
