#!/usr/bin/env bash
# Sourced by tools/ci-local.sh and tools/checks/ci-suite.sh: one suite at a
# time on this Mac. Two suites at once fight over QEMU ports and scratch
# files and fail at random, so the second one waits here instead.
#
# The lock is an atomic mkdir of /tmp/jt-ci-lock-<uid> holding a `pid`
# file. A lock whose pid is dead (kill -9, reboot) is cleared on sight. We
# wait up to 60 minutes, print one "waiting for ci-local pid N" line every
# 2 minutes, then give up. Released on EXIT/INT/TERM.
#   CI_LOCAL_NO_LOCK=1    skip the lock entirely
#   CI_LOCAL_LOCK_DIR     override the lock path (used by tests)
#   CI_LOCAL_LOCK_WAIT    max wait in seconds (default 3600)
#   CI_LOCAL_LOCK_POLL    poll interval in seconds (default 2)
# A child suite started by a process that already holds the lock inherits
# CI_LOCAL_LOCK_HELD=1 and does not lock again.

ci_lock_acquire() {
  [ "${CI_LOCAL_NO_LOCK:-}" = 1 ] && return 0
  [ "${CI_LOCAL_LOCK_HELD:-}" = 1 ] && return 0
  local dir="${CI_LOCAL_LOCK_DIR:-/tmp/jt-ci-lock-$(id -u)}"
  local max="${CI_LOCAL_LOCK_WAIT:-3600}"
  local poll="${CI_LOCAL_LOCK_POLL:-2}"
  local waited=0 last_msg=-120 pid
  while ! mkdir "$dir" 2>/dev/null; do
    pid=$(cat "$dir/pid" 2>/dev/null)
    if [ -z "$pid" ]; then
      # Holder is between mkdir and writing its pid, or died there.
      sleep 1
      pid=$(cat "$dir/pid" 2>/dev/null)
      if [ -z "$pid" ] && [ -d "$dir" ]; then
        ci_lock_clear_stale "$dir" "no pid"
        continue
      fi
    fi
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      ci_lock_clear_stale "$dir" "pid $pid"
      continue
    fi
    if [ "$waited" -ge "$max" ]; then
      echo "ci-local: gave up after $((waited / 60)) min waiting for ci-local pid ${pid:-?}; set CI_LOCAL_NO_LOCK=1 to run anyway, or rm -rf $dir if it is truly dead" >&2
      return 1
    fi
    if [ $((waited - last_msg)) -ge 120 ]; then
      echo "waiting for ci-local pid ${pid:-?}"
      last_msg=$waited
    fi
    sleep "$poll"
    waited=$((waited + poll))
  done
  echo $$ > "$dir/pid"
  CI_LOCK_DIR_HELD="$dir"
  export CI_LOCAL_LOCK_HELD=1
  trap ci_lock_release EXIT
  trap 'ci_lock_release; exit 130' INT
  trap 'ci_lock_release; exit 143' TERM
  return 0
}

ci_lock_clear_stale() {
  # mv is atomic, so of several waiters only one wins the stale dir.
  local dead="$1.stale.$$"
  echo "ci-local: clearing stale lock ($2 not alive)"
  mv "$1" "$dead" 2>/dev/null && rm -rf "$dead"
}

ci_lock_release() {
  if [ -n "${CI_LOCK_DIR_HELD:-}" ]; then
    if [ "$(cat "$CI_LOCK_DIR_HELD/pid" 2>/dev/null)" = "$$" ]; then
      rm -rf "$CI_LOCK_DIR_HELD"
    fi
    CI_LOCK_DIR_HELD=
  fi
}
