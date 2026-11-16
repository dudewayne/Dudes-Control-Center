#!/bin/bash
# Start the fan control daemon

echo "Stopping any existing daemon instances..."
pkill -9 dudes-control-center-fan-daemon 2>/dev/null
sleep 1

echo "Starting dudes-control-center-fan-daemon with sudo..."
cd "$(dirname "$0")"
sudo ./dudes-control-center-fan-daemon --foreground "$@"
