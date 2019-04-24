/*
 * Copyright (C) 2019 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include <common/compat/directory-handle.h>
#include <common/error.h>
#include <common/macros.h>
#include <common/runas.h>
#include <common/credentials.h>
#include <lttng/constant.h>

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static
int lttng_directory_handle_stat(const struct lttng_directory_handle *handle,
		const char *path, struct stat *st);
static
int lttng_directory_handle_mkdir(
		const struct lttng_directory_handle *handle,
		const char *path, mode_t mode);
static
int _run_as_mkdir(const struct lttng_directory_handle *handle, const char *path,
		mode_t mode, uid_t uid, gid_t gid);
static
int _run_as_mkdir_recursive(const struct lttng_directory_handle *handle,
		const char *path, mode_t mode, uid_t uid, gid_t gid);

#ifdef COMPAT_DIRFD

LTTNG_HIDDEN
int lttng_directory_handle_init(struct lttng_directory_handle *handle,
		const char *path)
{
	int ret;

	if (!path) {
		handle->dirfd = AT_FDCWD;
		ret = 0;
		goto end;
	}
	ret = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (ret == -1) {
		PERROR("Failed to initialize directory handle to \"%s\"", path);
		goto end;
	}
	handle->dirfd = ret;
	ret = 0;
end:
	return ret;
}

LTTNG_HIDDEN
int lttng_directory_handle_init_from_dirfd(
		struct lttng_directory_handle *handle, int dirfd)
{
	handle->dirfd = dirfd;
	return 0;
}

LTTNG_HIDDEN
void lttng_directory_handle_fini(struct lttng_directory_handle *handle)
{
	int ret;

	if (handle->dirfd == AT_FDCWD) {
		return;
	}
	ret = close(handle->dirfd);
	if (ret == -1) {
		PERROR("Failed to close directory file descriptor of directory handle");
	}
}

static
int lttng_directory_handle_stat(const struct lttng_directory_handle *handle,
		const char *path, struct stat *st)
{
	return fstatat(handle->dirfd, path, st, 0);
}

static
int lttng_directory_handle_mkdir(
		const struct lttng_directory_handle *handle,
		const char *path, mode_t mode)
{
	return mkdirat(handle->dirfd, path, mode);
}

static
int _run_as_mkdir(const struct lttng_directory_handle *handle, const char *path,
		mode_t mode, uid_t uid, gid_t gid)
{
	return run_as_mkdirat(handle->dirfd, path, mode, uid, gid);
}

static
int _run_as_mkdir_recursive(const struct lttng_directory_handle *handle,
		const char *path, mode_t mode, uid_t uid, gid_t gid)
{
	return run_as_mkdirat_recursive(handle->dirfd, path, mode, uid, gid);
}

#else /* COMPAT_DIRFD */

LTTNG_HIDDEN
int lttng_directory_handle_init(struct lttng_directory_handle *handle,
		const char *path)
{
	int ret;
	size_t cwd_len, path_len, handle_path_len;
	char cwd_buf[LTTNG_PATH_MAX];
	const char *cwd;
	bool add_slash = false;
	struct stat stat_buf;

	cwd = getcwd(cwd_buf, sizeof(cwd_buf));
	if (!cwd) {
		PERROR("Failed to initialize directory handle, can't get current working directory");
		ret = -1;
		goto end;
	}
	cwd_len = strlen(cwd);
	if (cwd_len == 0) {
		ERR("Failed to initialize directory handle to \"%s\": getcwd() returned an empty string",
				path);
		ret = -1;
		goto end;
	}
	if (cwd[cwd_len - 1] != '/') {
		add_slash = true;
	}

	if (path) {
		path_len = strlen(path);
		if (path_len == 0) {
			ERR("Failed to initialize directory handle: provided path is an empty string");
			ret = -1;
			goto end;
		}

		/*
		 * Ensure that 'path' is a directory. There is a race
		 * (TOCTOU) since the directory could be removed/replaced/renamed,
		 * but this is inevitable on platforms that don't provide dirfd support.
		 */
		ret = stat(path, &stat_buf);
		if (ret == -1) {
			PERROR("Failed to initialize directory handle to \"%s\", stat() failed",
			       path);
			goto end;
		}
		if (!S_ISDIR(stat_buf.st_mode)) {
			ERR("Failed to initialize directory handle to \"%s\": not a directory",
			    path);
			ret = -1;
			goto end;
		}
		if (*path == '/') {
			handle->base_path = strdup(path);
			if (!handle->base_path) {
				ret = -1;
			}
			/* Not an error. */
			goto end;
		}
	} else {
		path = "";
		path_len = 0;
		add_slash = false;
	}

	handle_path_len = cwd_len + path_len + !!add_slash + 2;
	if (handle_path_len >= LTTNG_PATH_MAX) {
		ERR("Failed to initialize directory handle as the resulting path's length (%zu bytes) exceeds the maximal allowed length (%d bytes)",
				handle_path_len, LTTNG_PATH_MAX);
		ret = -1;
		goto end;
	}
	handle->base_path = zmalloc(handle_path_len);
	if (!handle->base_path) {
		PERROR("Failed to initialize directory handle");
		ret = -1;
		goto end;
	}

	ret = sprintf(handle->base_path, "%s%s%s/", cwd,
			add_slash ? "/" : "", path);
	if (ret == -1 || ret >= handle_path_len) {
		ERR("Failed to initialize directory handle: path formatting failed");
		ret = -1;
		goto end;
	}
end:
	return ret;
}

LTTNG_HIDDEN
int lttng_directory_handle_init_from_dirfd(
		struct lttng_directory_handle *handle, int dirfd)
{
	assert(dirfd == AT_FDCWD);
	return lttng_directory_handle_init(handle, NULL);
}

LTTNG_HIDDEN
void lttng_directory_handle_fini(struct lttng_directory_handle *handle)
{
	free(handle->base_path);
}

static
int get_full_path(const struct lttng_directory_handle *handle,
		const char *subdirectory, char *fullpath, size_t size)
{
	int ret;

	/*
	 * Don't include the base path if subdirectory is absolute.
	 * This is the same behaviour than mkdirat.
	 */
	ret = snprintf(fullpath, size, "%s%s",
			*subdirectory != '/' ? handle->base_path : "",
			subdirectory);
	if (ret == -1 || ret >= size) {
		ERR("Failed to format subdirectory from directory handle");
		ret = -1;
	}
	ret = 0;
	return ret;
}

static
int lttng_directory_handle_stat(const struct lttng_directory_handle *handle,
		const char *subdirectory, struct stat *st)
{
	int ret;
	char fullpath[LTTNG_PATH_MAX];

	ret = get_full_path(handle, subdirectory, fullpath, sizeof(fullpath));
	if (ret) {
		errno = ENOMEM;
		goto end;
	}

	ret = stat(fullpath, st);
end:
	return ret;
}

static
int lttng_directory_handle_mkdir(const struct lttng_directory_handle *handle,
		const char *subdirectory, mode_t mode)
{
	int ret;
	char fullpath[LTTNG_PATH_MAX];

	ret = get_full_path(handle, subdirectory, fullpath, sizeof(fullpath));
	if (ret) {
		errno = ENOMEM;
		goto end;
	}

	ret = mkdir(fullpath, mode);
end:
	return ret;
}

static
int _run_as_mkdir(const struct lttng_directory_handle *handle, const char *path,
		mode_t mode, uid_t uid, gid_t gid)
{
	int ret;
	char fullpath[LTTNG_PATH_MAX];

	ret = get_full_path(handle, path, fullpath, sizeof(fullpath));
	if (ret) {
		errno = ENOMEM;
		goto end;
	}

	ret = run_as_mkdir(fullpath, mode, uid, gid);
end:
	return ret;
}

static
int _run_as_mkdir_recursive(const struct lttng_directory_handle *handle,
		const char *path, mode_t mode, uid_t uid, gid_t gid)
{
	int ret;
	char fullpath[LTTNG_PATH_MAX];

	ret = get_full_path(handle, path, fullpath, sizeof(fullpath));
	if (ret) {
		errno = ENOMEM;
		goto end;
	}

	ret = run_as_mkdir_recursive(fullpath, mode, uid, gid);
end:
	return ret;
}

#endif /* COMPAT_DIRFD */

/*
 * On some filesystems (e.g. nfs), mkdir will validate access rights before
 * checking for the existence of the path element. This means that on a setup
 * where "/home/" is a mounted NFS share, and running as an unpriviledged user,
 * recursively creating a path of the form "/home/my_user/trace/" will fail with
 * EACCES on mkdir("/home", ...).
 *
 * Checking the path for existence allows us to work around this behaviour.
 */
static
int create_directory_check_exists(const struct lttng_directory_handle *handle,
		const char *path, mode_t mode)
{
	int ret = 0;
	struct stat st;

	ret = lttng_directory_handle_stat(handle, path, &st);
	if (ret == 0) {
		if (S_ISDIR(st.st_mode)) {
			/* Directory exists, skip. */
			goto end;
		} else {
			/* Exists, but is not a directory. */
			errno = ENOTDIR;
			ret = -1;
			goto end;
		}
	}

	/*
	 * Let mkdir handle other errors as the caller expects mkdir
	 * semantics.
	 */
	ret = lttng_directory_handle_mkdir(handle, path, mode);
end:
	return ret;
}

/* Common implementation. */
static
int create_directory_recursive(const struct lttng_directory_handle *handle,
		const char *path, mode_t mode)
{
	char *p, tmp[LTTNG_PATH_MAX];
	size_t len;
	int ret;

	assert(path);

	ret = lttng_strncpy(tmp, path, sizeof(tmp));
	if (ret) {
		ERR("Failed to create directory: provided path's length (%zu bytes) exceeds the maximal allowed length (%zu bytes)",
				strlen(path) + 1, sizeof(tmp));
		goto error;
	}

	len = strlen(path);
	if (tmp[len - 1] == '/') {
		tmp[len - 1] = 0;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (tmp[strlen(tmp) - 1] == '.' &&
					tmp[strlen(tmp) - 2] == '.' &&
					tmp[strlen(tmp) - 3] == '/') {
				ERR("Using '/../' is not permitted in the trace path (%s)",
						tmp);
				ret = -1;
				goto error;
			}
			ret = create_directory_check_exists(handle, tmp, mode);
			if (ret < 0) {
				if (errno != EACCES) {
					PERROR("Failed to create directory \"%s\"",
							path);
					ret = -errno;
					goto error;
				}
			}
			*p = '/';
		}
	}

	ret = create_directory_check_exists(handle, tmp, mode);
	if (ret < 0) {
		PERROR("mkdirat recursive last element");
		ret = -errno;
	}
error:
	return ret;
}

LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_as_user(
		const struct lttng_directory_handle *handle,
		const char *subdirectory,
		mode_t mode, struct lttng_credentials *creds)
{
	int ret;

	if (!creds) {
		/* Run as current user. */
		ret = create_directory_check_exists(handle,
				subdirectory, mode);
	} else {
		ret = _run_as_mkdir(handle, subdirectory,
				mode, creds->uid, creds->gid);
	}

	return ret;
}

LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_recursive_as_user(
		const struct lttng_directory_handle *handle,
		const char *subdirectory_path,
		mode_t mode, struct lttng_credentials *creds)
{
	int ret;

	if (!creds) {
		/* Run as current user. */
		ret = create_directory_recursive(handle,
				subdirectory_path, mode);
	} else {
		ret = _run_as_mkdir_recursive(handle, subdirectory_path,
				mode, creds->uid, creds->gid);
	}

	return ret;
}

LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory(
		const struct lttng_directory_handle *handle,
		const char *subdirectory,
		mode_t mode)
{
	return lttng_directory_handle_create_subdirectory_as_user(
			handle, subdirectory, mode, NULL);
}

LTTNG_HIDDEN
int lttng_directory_handle_create_subdirectory_recursive(
		const struct lttng_directory_handle *handle,
		const char *subdirectory_path,
		mode_t mode)
{
	return lttng_directory_handle_create_subdirectory_recursive_as_user(
			handle, subdirectory_path, mode, NULL);
}
