#!/bin/bash
# dudes-control-center-hard-limit-enforce.sh
# Fail-safe enforcement script for CPU hard frequency limit.
# Reads the hard limit from profiles.json and writes it to sysfs.
# Runs independently of the daemon via systemd timer.

PROFILES_FILE="$HOME/.config/dudes-control-center/profiles.json"
LOG_FILE="$HOME/.cache/dudes-control-center/logs/hard-limit-enforcer.log"

# Ensure log directory exists
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null

log() {
    echo "[$(date '+%H:%M:%S')] $*" >> "$LOG_FILE" 2>/dev/null
}

# Check if profiles.json exists
if [ ! -f "$PROFILES_FILE" ]; then
    log "ERROR: profiles.json not found at $PROFILES_FILE"
    exit 1
fi

# Extract cpu_hard_limit_hz from profiles.json using grep/sed
HARD_LIMIT_HZ=$(grep -o '"cpu_hard_limit_hz"[[:space:]]*:[[:space:]]*[0-9]*' "$PROFILES_FILE" 2>/dev/null | grep -o '[0-9]*')

if [ -z "$HARD_LIMIT_HZ" ]; then
    log "ERROR: Could not read cpu_hard_limit_hz from profiles.json"
    exit 1
fi

# Convert Hz to kHz (sysfs uses kHz)
HARD_LIMIT_KHZ=$((HARD_LIMIT_HZ / 1000))
log "Enforcing hard limit: ${HARD_LIMIT_HZ} Hz (${HARD_LIMIT_KHZ} kHz)"

# Write to all policy directories
ENFORCED=0
FAILED=0
for policy_dir in /sys/devices/system/cpu/cpufreq/policy*; do
    if [ -f "$policy_dir/scaling_max_freq" ]; then
        current_max=$(cat "$policy_dir/scaling_max_freq" 2>/dev/null)
        if [ -n "$current_max" ] && [ "$current_max" -gt "$HARD_LIMIT_KHZ" ]; then
            echo "$HARD_LIMIT_KHZ" > "$policy_dir/scaling_max_freq" 2>/dev/null
            if [ $? -eq 0 ]; then
                ENFORCED=$((ENFORCED + 1))
            else
                FAILED=$((FAILED + 1))
                log "WARN: Failed to write max freq to $policy_dir"
            fi
        fi
    fi
done

log "Done: enforced=$ENFORCED, failed=$FAILED"
exit 0
