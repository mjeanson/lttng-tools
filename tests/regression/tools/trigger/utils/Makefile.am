# SPDX-License-Identifier: GPL-2.0-only

AM_CFLAGS += -I$(srcdir) -I$(top_srcdir)/tests/utils
LIBLTTNG_CTL=$(top_builddir)/src/lib/lttng-ctl/liblttng-ctl.la

noinst_PROGRAMS = \
	notification-client \
	register-some-triggers

notification_client_SOURCES = notification-client.c
notification_client_LDADD = $(LIBLTTNG_CTL) \
	$(top_builddir)/tests/utils/libtestutils.la

register_some_triggers_SOURCES = register-some-triggers.c
register_some_triggers_LDADD = $(LIBLTTNG_CTL) \
	$(top_builddir)/src/common/filter/libfilter.la \
	$(top_builddir)/src/common/bytecode/libbytecode.la
