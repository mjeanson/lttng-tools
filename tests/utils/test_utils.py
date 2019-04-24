#!/usr/bin/env python3
#
# Copyright (C) 2013 Jérémie Galarneau <jeremie.galarneau@efficios.com>
#
# SPDX-License-Identifier: GPL-2.0-only
#

import uuid
import os
import subprocess
import shutil
import sys
import tempfile

# Import lttng bindings generated in the current tree
lttng_bindings_path = os.path.dirname(os.path.abspath(__file__)) + "/"
for i in range(3):
    lttng_bindings_path = os.path.dirname(lttng_bindings_path)
lttng_bindings_path = lttng_bindings_path + "/extras/bindings/swig/python"
lttng_bindings_libs_path = lttng_bindings_path + "/.libs"
sys.path.append(lttng_bindings_path)
sys.path.append(lttng_bindings_libs_path)
from lttng import *


class SessionInfo:
    def __init__(self, handle, session_name, tmp_directory, channel_name):
        self.handle = handle
        self.name = session_name
        self.tmp_directory = tmp_directory
        self.trace_path = tmp_directory + "/" + session_name
        self.channel_name = channel_name

def bail(diag, session_info = None):
    print("Bail out!")
    print("#", diag)

    if session_info is not None:
        stop_session(session_info, True)

        if os.path.exists(session_info.tmp_directory):
            shutil.rmtree(session_info.tmp_directory)
    exit(-1)

def print_test_result(result, number, description):
    result_string = None
    if result is True:
        result_string = "ok"
    else:
        result_string = "not ok"

    result_string += " {0} - {1}".format(number, description)
    print(result_string)

def skip_test(number, description):
    print('ok {} # skip {}'.format(number, description))

def enable_ust_tracepoint_event(session_info, event_name):
    event = Event()
    event.name = event_name
    event.type = EVENT_TRACEPOINT
    event.loglevel = EVENT_LOGLEVEL_ALL
    res = enable_event(session_info.handle, event, session_info.channel_name)
    if res < 0:
        bail("Failed to enable userspace event " + event_name, session_info)

def create_session():
    dom = Domain()
    dom.type = DOMAIN_UST

    session_name = str(uuid.uuid1())
    tmp_directory = tempfile.mkdtemp()
    trace_path = tmp_directory + "/" + session_name

    res = create(session_name, trace_path)
    if res < 0:
        bail("Failed to create tracing session.")

    channel = Channel()
    channel.name = "channel0"
    channel_set_default_attr(dom, channel.attr)

    han = Handle(session_name, dom)
    res = enable_channel(han, channel)

    session_info = SessionInfo(han, session_name, tmp_directory, channel.name)
    if res < 0:
        bail("Failed to enable channel " + channel.name, session_info)
    return session_info

def start_session(session_info):
    start(session_info.name)

def stop_session(session_info, bailing = False):
    # Workaround lttng-ctl outputing directly to stdout by spawning a subprocess.
    lttng_binary_path = os.path.dirname(os.path.abspath(__file__)) + "/"
    for i in range(3):
        lttng_binary_path = os.path.dirname(lttng_binary_path)
    lttng_binary_path = lttng_binary_path + "/src/bin/lttng/lttng"

    retcode = subprocess.call([lttng_binary_path, "stop", session_info.name], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if retcode != 0 and not bailing:
        bail("Unable to stop session " + session_info.name, session_info)
    destroy(session_info.name)
