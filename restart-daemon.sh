#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Stop, update, and restart the dcc-fan-daemon service with proper cleanup

echo "=== Restarting Dudes Control Center Fan Daemon ==="
echo ""

echo "1. Stopping service..."
systemctl stop dcc-fan-daemon.service

echo "2. Force killing any remaining daemon processes..."
pkill -9 -f dcc-fan-daemon 2>/dev/null || echo "   No additional processes found"

sleep 1

echo "3. Updating systemd service file..."
cp "$(dirname "$(readlink -f "$0")")/dcc-fan-daemon.service" /etc/systemd/system/dcc-fan-daemon.service
systemctl daemon-reload
echo "   ✓ Service file updated"

sleep 1

echo "4. Starting service..."
systemctl start dcc-fan-daemon.service

sleep 2

echo "5. Service status:"
systemctl is-active dcc-fan-daemon.service && echo "   ✓ Service is running" || echo "   ✗ Service failed to start"

echo ""
echo "6. Checking daemon process..."
ps aux | grep -E "dcc-fan-daemon.*--foreground" | grep -v grep || echo "   No daemon process found!"

echo ""
echo "7. Testing profile list via socket..."
if [ -S /run/dcc-fan.sock ]; then
    (exec 3<>/run/dcc-fan.sock && echo "LIST_PROFILES" >&3 && cat <&3)
else
    echo "   Socket not found at /run/dcc-fan.sock"
fi

echo ""
echo "=== Restart complete ==="