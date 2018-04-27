##
## The version.i file must be verified and generated or updated if the
## git commit id (called git version.i here) changed since the last build
## of lttng-tools.
##
version.i:
	$(AM_V_at)rm -f version.i.tmp; \
	if test -r "$(top_srcdir)/.git" && \
			test -x "`which git 2>&1;true`"; then \
		GIT_VERSION_STR="`cd "$(top_srcdir)" && git describe --tags --dirty`"; \
		echo "#define GIT_VERSION \"$$GIT_VERSION_STR\"" > version.i.tmp; \
	fi; \
	if test ! -f version.i.tmp; then \
		if test ! -f version.i; then \
			echo '#define GIT_VERSION ""' > version.i; \
		fi; \
	elif test ! -f version.i || \
			test x"`cat version.i.tmp`" != x"`cat version.i`"; then \
		mv version.i.tmp version.i; \
	fi; \
	rm -f version.i.tmp; \
	true

##
## version.i is defined as a .PHONY file even if it's a real file as
## we want our routine to be ran for each build.
##
.PHONY: version.i

CLEANFILES = version.i.tmp
DISTCLEANFILES = version.i

lttnginclude_HEADERS = \
	lttng/health.h \
	lttng/lttng.h \
	lttng/constant.h \
	lttng/channel.h \
	lttng/domain.h \
	lttng/event.h \
	lttng/handle.h \
	lttng/session.h \
	lttng/lttng-error.h \
	lttng/snapshot.h \
	lttng/save.h \
	lttng/load.h \
	lttng/endpoint.h \
	lttng/rotation.h \
	lttng/location.h

lttngactioninclude_HEADERS= \
	lttng/action/action.h \
	lttng/action/notify.h

lttngconditioninclude_HEADERS= \
	lttng/condition/condition.h \
	lttng/condition/buffer-usage.h \
	lttng/condition/session-consumed-size.h \
	lttng/condition/evaluation.h

lttngnotificationinclude_HEADERS= \
	lttng/notification/channel.h \
	lttng/notification/notification.h

lttngtriggerinclude_HEADERS= \
	lttng/trigger/trigger.h

noinst_HEADERS = \
	lttng/snapshot-internal.h \
	lttng/health-internal.h \
	lttng/save-internal.h \
	lttng/load-internal.h \
	lttng/action/action-internal.h \
	lttng/action/notify-internal.h \
	lttng/condition/condition-internal.h \
	lttng/condition/buffer-usage-internal.h \
	lttng/condition/session-consumed-size-internal.h \
	lttng/condition/evaluation-internal.h \
	lttng/notification/notification-internal.h \
	lttng/trigger/trigger-internal.h \
	lttng/endpoint-internal.h \
	lttng/notification/channel-internal.h \
	lttng/channel-internal.h \
	lttng/rotate-internal.h \
	lttng/ref-internal.h \
	lttng/location-internal.h \
	version.h \
	version.i
