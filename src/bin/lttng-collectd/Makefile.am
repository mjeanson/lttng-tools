if HAVE_LIBLTTNG_UST_CTL
lttnglibexec_PROGRAMS = lttng-collectd

lttng_collectd_SOURCES = lttng-collectd.c \
	lttng-collectd.h

lttng_collectd_LDADD = \
	$(top_builddir)/src/common/compat/libcompat.la \
	$(top_builddir)/src/common/libcommon.la

lttng_collectd_LDADD += -llttng-ust -ldl
endif
