#!/bin/sh
set -eu
ulimit -c 0

cd "$(dirname "$0")"
work="$(mktemp -d "${TMPDIR:-/tmp}/nickeltypefix-log.XXXXXX")"
trap 'rm -rf "$work"' EXIT HUP INT TERM

# The test replaces only NickelHook's syslog sink; file writes use the production logger.
printf '%s\n' 'void nh_log(const char *fmt, ...);' > "$work/NickelHook.h"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -pthread -I"$work" \
    "-DNTF_CONFIG_DIR=\"$work/log\"" log_test.c -o "$work/log-test"
"$work/log-test"
