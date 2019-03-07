/*
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: MIT
 *
 */

#ifndef LTTNG_UUID_H
#define LTTNG_UUID_H

#include <common/macros.h>

/*
 * Includes final \0.
 */
#define UUID_STR_LEN		37
#define UUID_LEN		16

#ifdef LTTNG_HAVE_LIBUUID
#include <uuid/uuid.h>

/*
 * uuid_out is of len UUID_LEN.
 */
static inline
int lttng_uuid_generate(unsigned char *uuid_out)
{
	uuid_generate(uuid_out);
	return 0;
}

#elif defined(LTTNG_HAVE_LIBC_UUID)
#include <uuid.h>
#include <stdint.h>

/*
 * uuid_out is of len UUID_LEN.
 */
static inline
int lttng_uuid_generate(unsigned char *uuid_out)
{
	uint32_t status;

	uuid_create((uuid_t *) uuid_out, &status);
	if (status == uuid_s_ok)
		return 0;
	else
		return -1;
}

#else
#error "LTTng-Tools needs to have a UUID generator configured."
#endif

/*
 * Convert a UUID to a human-readable, NULL-terminated, string of the form
 * xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx.
 *
 * Assumes uuid_str is at least UUID_STR_LEN byte long.
 */
LTTNG_HIDDEN
void lttng_uuid_to_str(const unsigned char *uuid, char *uuid_str);

#endif /* LTTNG_UUID_H */
