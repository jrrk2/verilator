#!/bin/sh
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
