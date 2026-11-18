#!/bin/sh
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
# ar with the member list in a response file: MinGW's ar.exe is a native program, and
# libabc.a has more objects than fit in a Windows command line (32 KiB)
op=$1 lib=$2
shift 2
rsp=$(mktemp)
printf '%s\n' "$@" > "$rsp"
ar "$op" "$lib" @"$rsp"
status=$?
rm -f "$rsp"
exit $status
