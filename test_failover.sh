#!/bin/bash
# Test script for failover functionality

echo "=== Failover Test Script ==="
echo ""

echo "Step 1: Clean up any running processes"
pkill -9 bin_nm bin_ss bin_client 2>/dev/null
sleep 1

echo ""
echo "Step 2: Start NM"
./bin_nm &
NM_PID=$!
sleep 2

echo ""
echo "Step 3: Start backup SS (ss6_backup)"
./bin_ss storage_ss6_backup 9003 9004 &
SS_BACKUP_PID=$!
sleep 2

echo ""
echo "Step 4: Start primary SS (ss6)"
./bin_ss storage_ss6 9001 9002 &
SS_PRIMARY_PID=$!
sleep 2

echo ""
echo "Step 5: Create and write a test file"
./bin_client CREATE nahi raju
echo "test content" | ./bin_client WRITE nahi raju
sleep 2

echo ""
echo "Step 6: Verify READ works with primary"
./bin_client READ nahi raju
echo ""

echo "Step 7: Kill primary SS (ss6)"
echo "Killing process $SS_PRIMARY_PID"
kill -9 $SS_PRIMARY_PID
echo "Primary killed at: $(date '+%H:%M:%S')"
echo ""

echo "Step 8: Wait for failover (need 3 missed heartbeats)"
echo "Timeout=15s, Check interval=5s, Missed count=3"
echo "Waiting 20 seconds for failover to complete..."
for i in {1..20}; do
    echo -n "."
    sleep 1
done
echo ""
echo "Failover should be complete at: $(date '+%H:%M:%S')"
echo ""

echo "Step 9: Check NM logs for failover"
grep -E "failover" nm.log | tail -10
echo ""

echo "Step 10: Try READ again (should use backup)"
./bin_client READ nahi raju
echo ""

echo "=== Test Complete ==="
echo "Check nm.log for detailed failover logs"
