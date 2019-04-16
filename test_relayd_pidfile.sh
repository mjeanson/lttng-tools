#!/bin/bash -u

signal=SIGTERM
if [ "x${1:-}" != "x" ]; then
	signal=${1:-}
fi

RELAYD="src/bin/lttng-relayd/lttng-relayd"

pid_file="$(mktemp -u)"

relay_exited=0
$RELAYD -p "$pid_file" &
ret=$?
pid=$!

if [ $ret = 0 ]; then
	echo "relayd started with pid: $pid"

	while true; do
		if [ "$(ps -p $pid -o pid=)" = "" ]; then
			echo "relayd already exited..."
			relay_exited=1
			break
		fi

		echo "waiting for relayd... ($pid_file)"
		test -f "$pid_file" && break
		sleep 0.5
	done

	if [ "$relay_exited" != 1 ]; then
		pid_from_file=$(cat "$pid_file")
		echo "relayd started with pid file: $pid_from_file ($pid_file)"

		echo -n "pids match: "
		if [ "$pid" == "$pid_from_file" ]; then
			echo yes
		else
			echo no
		fi

		kill -s "$signal" $pid
		ret=$?
		echo "kill $signal relayd: $ret"
		wait $pid
	fi

	echo -n "cleaned up pid file: "
	if [ -f "$pid_file" ]; then
		echo no
	else
		echo yes
	fi
fi
