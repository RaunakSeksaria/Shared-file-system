#!/bin/bash
# Test script for heartbeat monitoring and failure detection

echo "=== Testing Heartbeat Monitoring and Failure Detection ==="
echo ""

# Cleanup previous runs
pkill -f bin_nm
pkill -f bin_ss
sleep 1

# Start Name Server
echo "[1] Starting Name Server..."
./bin_nm --host 127.0.0.1 --port 5000 &
NM_PID=$!
sleep 2

# Start Storage Server 1
echo "[2] Starting Storage Server 1 (ss1)..."
./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 6001 \
         --storage storage_ss1 --username ss1 &
SS1_PID=$!
sleep 2

# Start Storage Server 2
echo "[3] Starting Storage Server 2 (ss2)..."
./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 6002 \
         --storage storage_ss2 --username ss2 &
SS2_PID=$!
sleep 2

echo ""
echo "[4] Both servers registered and sending heartbeats"
echo "    SS1 PID: $SS1_PID"
echo "    SS2 PID: $SS2_PID"
echo ""
echo "[5] Waiting 10 seconds for heartbeats to stabilize..."
sleep 10

echo ""
echo "[6] Killing SS1 to simulate failure..."
kill -9 $SS1_PID
echo "    SS1 terminated"
echo ""

echo "[7] Waiting 20 seconds for NM to detect failure..."
echo "    (Timeout is 15 seconds, so should detect around 15-20 seconds)"
for i in {1..20}; do
    echo -n "."
    sleep 1
done
echo ""
echo ""

echo "[8] Check NM logs for failure detection:"
echo "    Look for 'SS ss1 marked as FAILED' message"
echo ""

echo "[9] SS2 should still be alive and sending heartbeats"
echo ""
sleep 5

echo "[10] Cleanup: Stopping all processes..."
kill $NM_PID 2>/dev/null
kill $SS2_PID 2>/dev/null
sleep 1

echo ""
echo "=== Test Complete ==="
echo "Review the logs above to verify:"
echo "  - SS1 registered and sent heartbeats"
echo "  - SS2 registered and sent heartbeats"
echo "  - SS1 failure was detected after ~15-20 seconds"
echo "  - SS2 continued to function normally"
