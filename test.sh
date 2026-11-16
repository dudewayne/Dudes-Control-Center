#!/bin/bash
# Comprehensive test script for Dudes Control Center applications
# Tests hardware access, daemon startup, socket permissions, and GUI connectivity

set -e

echo "=========================================="
echo "  Dudes Control Center - Test Suite"
echo "=========================================="
echo ""

CLI="./dudes-control-center-fan-cli"
DAEMON="./dudes-control-center-fan-daemon"
GUI="./dudes-control-center-gui"
SOCKET_PATH="/tmp/dudes-control-center-fan.sock"

PASS=0
FAIL=0

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    ((PASS++))
}

fail() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    ((FAIL++))
}

warn() {
    echo -e "${YELLOW}⚠ WARN${NC}: $1"
}

# Check if binaries exist
echo "=== Checking Binaries ==="
for binary in "$CLI" "$DAEMON" "$GUI"; do
    if [ -f "$binary" ] && [ -x "$binary" ]; then
        SIZE=$(ls -lh "$binary" | awk '{print $5}')
        pass "$binary exists ($SIZE)"
    else
        fail "$binary not found or not executable"
    fi
done

echo ""
echo "=== Testing Help Commands (no sudo) ==="

# Test CLI help
if $CLI --help > /dev/null 2>&1; then
    pass "CLI help works"
else
    fail "CLI help failed"
fi

# Test daemon help
if $DAEMON --help > /dev/null 2>&1; then
    pass "Daemon help works"
else
    fail "Daemon help failed"
fi

echo ""
echo "=== Checking Root Privileges ==="

if [ "$EUID" -ne 0 ]; then
    warn "Running without root privileges"
    warn "Hardware tests will be skipped"
    echo ""
    echo "To test hardware access, run: sudo $0"
else
    pass "Running with root privileges"
fi

echo ""
echo "=== Testing Hardware Access ==="

if [ "$EUID" -eq 0 ]; then
    # Test CLI status (requires sudo)
    OUTPUT=$($CLI 2>&1 || true)
    
    if echo "$OUTPUT" | grep -q "CPU Temperature"; then
        pass "CLI can read hardware"
        echo "   Output preview:"
        echo "$OUTPUT" | head -5 | sed 's/^/   /'
        
        # Check for zero values (indicates failure)
        if echo "$OUTPUT" | grep -qE "(CPU|GPU) Temperature.*0 °C"; then
            warn "Temperature reading shows 0°C - sensor may not be working"
        fi
        
        if echo "$OUTPUT" | grep -qE "Fan Speed.*0 RPM"; then
            warn "Fan speed shows 0 RPM - sensor may not be working"
        fi
    else
        fail "CLI failed to read hardware"
        echo "   Error: $OUTPUT" | head -3 | sed 's/^/   /'
    fi
else
    warn "Skipping hardware test (requires sudo)"
fi

echo ""
echo "=== Testing Daemon Startup ==="

# Clean up any existing socket
rm -f "$SOCKET_PATH" 2>/dev/null || true

# Start daemon in background
$DAEMON --foreground > /tmp/daemon_test.log 2>&1 &
DAEMON_PID=$!
sleep 2

if kill -0 $DAEMON_PID 2>/dev/null; then
    pass "Daemon started (PID: $DAEMON_PID)"
    
    # Check daemon log for socket creation message
    if grep -q "Socket created" /tmp/daemon_test.log; then
        pass "Daemon reports socket created"
        grep "Socket" /tmp/daemon_test.log | sed 's/^/   /'
    else
        warn "Daemon log doesn't show socket creation message"
        cat /tmp/daemon_test.log | head -5 | sed 's/^/   /'
    fi
    
    # Check socket file exists
    if [ -S "$SOCKET_PATH" ]; then
        pass "Socket file created at $SOCKET_PATH"
        
        # Check permissions
        PERMS=$(stat -c "%a" "$SOCKET_PATH" 2>/dev/null)
        OWNER=$(stat -c "%U:%G" "$SOCKET_PATH" 2>/dev/null)
        
        echo "   Permissions: $PERMS (expected: 666)"
        echo "   Owner: $OWNER"
        
        if [ "$PERMS" = "666" ]; then
            pass "Socket permissions are correct (666)"
        else
            fail "Socket permissions are wrong ($PERMS, expected 666)"
        fi
        
        # Test socket connectivity with a simple client
        echo ""
        echo "=== Testing Socket Connection ==="
        
        if command -v socat > /dev/null 2>&1; then
            SOCKET_RESPONSE=$(echo "STATUS" | timeout 2 socat - UNIX-CONNECT:$SOCKET_PATH 2>/dev/null || true)
            
            if [ -n "$SOCKET_RESPONSE" ]; then
                pass "Socket is accessible via socat"
                echo "   Response: $SOCKET_RESPONSE" | head -3 | sed 's/^/   /'
            else
                fail "Socket connection failed (no response)"
            fi
        else
            warn "socat not installed, skipping socket test"
            warn "Install with: sudo apt install socat"
            
            # Try with a simple Python client as fallback
            if command -v python3 > /dev/null 2>&1; then
                SOCKET_RESPONSE=$(python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect('$SOCKET_PATH')
    s.send(b'STATUS\n')
    response = s.recv(4096).decode()
    print(response.strip())
except Exception as e:
    print(f'Error: {e}')
finally:
    s.close()
" 2>/dev/null || true)
                
                if [ -n "$SOCKET_RESPONSE" ] && ! echo "$SOCKET_RESPONSE" | grep -q "Error:"; then
                    pass "Socket is accessible via Python"
                    echo "   Response preview: $SOCKET_RESPONSE" | head -1 | sed 's/^/   /'
                else
                    fail "Socket connection failed (Python test)"
                    echo "   Error: $SOCKET_RESPONSE" | sed 's/^/   /'
                fi
            fi
        fi
        
    else
        fail "Socket file not found at $SOCKET_PATH"
        echo "   Daemon log:"
        cat /tmp/daemon_test.log | head -10 | sed 's/^/   /'
    fi
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
    rm -f "$SOCKET_PATH" 2>/dev/null || true
else
    fail "Daemon failed to start"
    echo "   Log:"
    cat /tmp/daemon_test.log | head -10 | sed 's/^/   /'
fi

echo ""
echo "=== Testing GUI Launch ==="

if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then
    # Try to launch GUI briefly
    timeout 2 $GUI > /dev/null 2>&1 &
    GUI_PID=$!
    sleep 1
    
    if kill -0 $GUI_PID 2>/dev/null; then
        pass "GUI launched successfully"
        kill $GUI_PID 2>/dev/null || true
    else
        warn "GUI failed to launch (may need X11/Wayland display)"
    fi
else
    warn "No display available, skipping GUI test"
fi

echo ""
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
echo -e "${GREEN}Passed: $PASS${NC}"
echo -e "${RED}Failed: $FAIL${NC}"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    echo ""
    echo "=== Usage Instructions ==="
    echo ""
    echo "1. Start the daemon in a terminal (requires sudo):"
    echo "   sudo $DAEMON --foreground"
    echo ""
    echo "2. In another terminal, launch the GUI:"  
    echo "   $GUI"
    echo ""
    echo "3. Or use CLI directly:"
    echo "   sudo $CLI              # Show status"
    echo "   sudo $CLI --set 50     # Set fan to 50%"
    echo "   sudo $CLI --auto       # Auto mode"
else
    echo -e "${RED}Some tests failed. Please review the output above.${NC}"
fi

exit $FAIL
