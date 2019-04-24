/*
 * Copyright (C) 2012 Christian Babeux <christian.babeux@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef NTESTPOINT

#define _LGPL_SOURCE
#include <dlfcn.h>  /* for dlsym   */
#include <stdlib.h> /* for getenv  */
#include <string.h> /* for strncmp */

#include "testpoint.h"

/* Environment variable used to enable the testpoints facilities. */
static const char *lttng_testpoint_env_var = "LTTNG_TESTPOINT_ENABLE";

/* Testpoint toggle flag */
int lttng_testpoint_activated;

/*
 * Toggle the support for testpoints on the application startup.
 */
static void __attribute__((constructor)) lttng_testpoint_check(void)
{
	char *testpoint_env_val = NULL;

	testpoint_env_val = getenv(lttng_testpoint_env_var);
	if (testpoint_env_val != NULL
			&& (strncmp(testpoint_env_val, "1", 1) == 0)) {
		lttng_testpoint_activated = 1;
	}
}

/*
 * Lookup a symbol by name.
 *
 * Return the address where the symbol is loaded or NULL if the symbol was not
 * found.
 */
void *lttng_testpoint_lookup(const char *name)
{
	if (!name) {
		return NULL;
	}

	return dlsym(RTLD_DEFAULT, name);
}

#endif /* NTESTPOINT */
