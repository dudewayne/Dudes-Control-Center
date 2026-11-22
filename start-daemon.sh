#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Start the fan control daemon

echo "Stopping any existing daemon instances..."
pkill -9 dcc-fan-daemon 2>/dev/null
sleep 1

echo "Starting dcc-fan-daemon with sudo..."
cd "$(dirname "$0")"
sudo ./dcc-fan-daemon --foreground "$@"
