/*
 * Copyright (C) 2017 Francis Deslauriers <francis.deslauriers@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#include <sys/sdt.h>
#include "foobar_provider.h"
void bar_function() {
	FOOBAR_TP_IN_DLOPEN();
}
