/*
 * Copyright (C) 2018 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_TESTAPP_SIGNAL_HELPER_H
#define LTTNG_TESTAPP_SIGNAL_HELPER_H

#include <signal.h>

static volatile int should_quit;

static
void sighandler(int sig)
{
	if (sig == SIGTERM) {
		should_quit = 1;
	}
}

static
int set_signal_handler(void)
{
	int ret;
	struct sigaction sa = {
		.sa_flags = 0,
		.sa_handler = sighandler,
	};

	ret = sigemptyset(&sa.sa_mask);
	if (ret) {
		perror("sigemptyset");
		goto end;
	}

	ret = sigaction(SIGTERM, &sa, NULL);
	if (ret) {
		perror("sigaction");
		goto end;
	}
end:
	return ret;
}

#endif /* LTTNG_TESTAPP_SIGNAL_HELPER_H */
