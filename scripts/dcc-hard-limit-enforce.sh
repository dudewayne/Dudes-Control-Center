#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# dcc-hard-limit-enforce.sh
# Fail-safe enforcement script for CPU hard frequency limit.
# Reads the hard limit from profiles.json and writes it to sysfs.
# Runs independently of the daemon via systemd timer (as root).
#
# Security: profiles.json lives in a user's home directory. This script runs
# as root, so it reads the file with the identity of the directory's owner
# (setpriv) and never writes anything into the home directory.

# Whose profiles.json: same order as the daemons (dcc_profiles_path):
# $DCC_PROFILES_PATH, the user in /etc/dudes-control-center/user, the first
# member of group dudes-control-center, the user with uid 1000.
DCC_USER_FILE="${DCC_USER_FILE:-/etc/dudes-control-center/user}"
if [ -n "${DCC_PROFILES_PATH:-}" ] && [ "${DCC_PROFILES_PATH#/}" != "$DCC_PROFILES_PATH" ]; then
    PROFILES_FILE="$DCC_PROFILES_PATH"
else
    DCC_USER=""
    [ -r "$DCC_USER_FILE" ] && DCC_USER=$(head -n 1 "$DCC_USER_FILE" | tr -d '[:space:]')
    [ -z "$DCC_USER" ] && DCC_USER=$(getent group dudes-control-center | cut -d: -f4 | cut -d, -f1)
    [ -z "$DCC_USER" ] && DCC_USER=$(getent passwd 1000 | cut -d: -f1)
    USER_HOME=$(getent passwd "$DCC_USER" | cut -d: -f6)
    PROFILES_FILE="$USER_HOME/.config/dudes-control-center/profiles.json"
fi
PROFILES_DIR=$(dirname "$PROFILES_FILE")
LOG_DIR="/var/log/dudes-control-center"
LOG_FILE="$LOG_DIR/HARD_LIMIT_ENFORCER.log"
LOG_MAX_BYTES=$((1024 * 1024))

mkdir -p "$LOG_DIR" 2>/dev/null

log() {
    # Simple size cap so a 10 s timer cannot fill the disk
    if [ -f "$LOG_FILE" ] && [ "$(stat -c %s "$LOG_FILE" 2>/dev/null || echo 0)" -gt "$LOG_MAX_BYTES" ]; then
        mv -f "$LOG_FILE" "$LOG_FILE.1" 2>/dev/null
    fi
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG_FILE" 2>/dev/null
}

# The config directory must be a real directory owned by a regular user
if [ -L "$PROFILES_DIR" ] || [ ! -d "$PROFILES_DIR" ]; then
    log "ERROR: $PROFILES_DIR is missing or a symlink"
    exit 1
fi
OWNER_UID=$(stat -c %u "$PROFILES_DIR")
OWNER_GID=$(stat -c %g "$PROFILES_DIR")
if [ "$OWNER_UID" = "0" ]; then
    log "ERROR: $PROFILES_DIR is owned by root; expected the desktop user"
    exit 1
fi

# Read the file as its owner; refuse to follow a symlink at the file itself
if [ -L "$PROFILES_FILE" ]; then
    log "ERROR: $PROFILES_FILE is a symlink - refusing to read it"
    exit 1
fi
CONTENT=$(setpriv --reuid="$OWNER_UID" --regid="$OWNER_GID" --clear-groups \
          head -c 1048576 -- "$PROFILES_FILE" 2>/dev/null)
if [ -z "$CONTENT" ]; then
    log "ERROR: profiles.json not readable at $PROFILES_FILE"
    exit 1
fi

# Extract cpu_hard_limit_hz (first match, digits only)
HARD_LIMIT_HZ=$(printf '%s' "$CONTENT" \
    | grep -o '"cpu_hard_limit_hz"[[:space:]]*:[[:space:]]*[0-9]*' \
    | head -n 1 | grep -o '[0-9]*$')

if ! [[ "$HARD_LIMIT_HZ" =~ ^[0-9]{6,12}$ ]]; then
    log "ERROR: Could not read a valid cpu_hard_limit_hz from profiles.json"
    exit 1
fi

# Convert Hz to kHz (sysfs uses kHz)
HARD_LIMIT_KHZ=$((HARD_LIMIT_HZ / 1000))

# Write to all policy directories
ENFORCED=0
FAILED=0
for policy_dir in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -f "$policy_dir/scaling_max_freq" ]; then
        current_max=$(cat "$policy_dir/scaling_max_freq" 2>/dev/null)
        if [ -n "$current_max" ] && [ "$current_max" -gt "$HARD_LIMIT_KHZ" ]; then
            if echo "$HARD_LIMIT_KHZ" > "$policy_dir/scaling_max_freq" 2>/dev/null; then
                ENFORCED=$((ENFORCED + 1))
            else
                FAILED=$((FAILED + 1))
                log "WARN: Failed to write max freq to $policy_dir"
            fi
        fi
    fi
done

# Only log when something happened, to keep the log readable
if [ "$ENFORCED" -gt 0 ] || [ "$FAILED" -gt 0 ]; then
    log "Hard limit ${HARD_LIMIT_HZ} Hz (${HARD_LIMIT_KHZ} kHz): enforced=$ENFORCED, failed=$FAILED"
fi
exit 0
