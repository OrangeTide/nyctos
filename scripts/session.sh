#!/bin/sh
# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Feed a recorded session to a machine's console and print what came back.
#
#   session.sh <delay> <qemu> <flags> <kernel>
#
# The delay is seconds between lines. Zero is a burst, which is how a pipe
# delivers a session and is not how a person types: every line arrives
# before anything has asked for one. A delay makes each line arrive while
# something is already waiting for it, which is the other path through the
# console and the one a burst never reaches.
#
# Nothing here interprets the session. It is the lines a person would type,
# and what the machine does with them is what the check compares.

set -u

delay="$1"
qemu="$2"
flags="$3"
kernel="$4"

session="${SESSION:-share/check/session.txt}"

feed() {
    if [ "$delay" != "0" ]; then
        sleep "$delay"
    fi
    while IFS= read -r line; do
        printf '%s\n' "$line"
        if [ "$delay" != "0" ]; then
            sleep "$delay"
        fi
    done < "$session"

    # The machine halts on its own once the shell leaves. This is only so
    # that a machine which does not halt is killed by the timeout rather
    # than waiting on a pipe that never closes.
    sleep 5
}

# shellcheck disable=SC2086
feed | timeout "${TIMEOUT:-360}" $qemu $flags -kernel "$kernel" 2>&1
