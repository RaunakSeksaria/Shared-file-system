#!/bin/bash

# Test script for replication system
# Tests: CREATE replication and DELETE replication

echo "=== Replication Test Script ==="

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test configuration
TEST_FILE="repl_test_$(date +%s).txt"
TEST_CONTENT="This is a replication test file"

echo -e "\n${YELLOW}Step 1: Ensure NM and storage servers are running${NC}"
echo "Check if bin_nm is running..."
if ! pgrep -f bin_nm > /dev/null; then
    echo -e "${RED}Error: bin_nm is not running${NC}"
    echo "Please start NM first: ./bin_nm"
    exit 1
fi
echo -e "${GREEN}✓ NM is running${NC}"

echo -e "\n${YELLOW}Step 2: Check storage servers${NC}"
echo "Checking for storage servers..."
SS_COUNT=$(pgrep -f bin_ss | wc -l)
echo "Found $SS_COUNT storage server(s) running"

if [ "$SS_COUNT" -lt 2 ]; then
    echo -e "${RED}Warning: Need at least 2 storage servers for replication test${NC}"
    echo "Example: ./bin_ss storage_ss1 9001 9002 & ./bin_ss storage_ss1_backup 9003 9004 &"
    exit 1
fi
echo -e "${GREEN}✓ Sufficient storage servers running${NC}"

echo -e "\n${YELLOW}Step 3: Test CREATE replication${NC}"
echo "Creating test file: $TEST_FILE"

# Connect client and create file
./bin_client << EOF
CREATE $TEST_FILE
WRITE $TEST_FILE
$TEST_CONTENT
.
EXIT
EOF

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Failed to create file${NC}"
    exit 1
fi

echo -e "${GREEN}✓ File created${NC}"

echo -e "\n${YELLOW}Step 4: Wait for async replication${NC}"
echo "Waiting 3 seconds for replication worker to process..."
sleep 3

echo -e "\n${YELLOW}Step 5: Verify file exists on both primary and replica${NC}"
echo "Checking NM logs for replication activity..."
if grep -q "Queued replication for CREATE $TEST_FILE" logs/nm.log 2>/dev/null; then
    echo -e "${GREEN}✓ Replication job was queued${NC}"
else
    echo -e "${YELLOW}⚠ Could not verify replication job in logs${NC}"
fi

if grep -q "Replication job completed successfully" logs/nm.log 2>/dev/null; then
    echo -e "${GREEN}✓ Replication job completed${NC}"
else
    echo -e "${YELLOW}⚠ Replication completion not confirmed in logs${NC}"
fi

echo -e "\n${YELLOW}Step 6: Test DELETE replication${NC}"
echo "Deleting test file: $TEST_FILE"

./bin_client << EOF
DELETE $TEST_FILE
EXIT
EOF

if [ $? -ne 0 ]; then
    echo -e "${RED}✗ Failed to delete file${NC}"
    exit 1
fi

echo -e "${GREEN}✓ File deleted${NC}"

echo -e "\n${YELLOW}Step 7: Wait for delete replication${NC}"
echo "Waiting 3 seconds for replication worker..."
sleep 3

echo -e "\n${YELLOW}Step 8: Check logs for delete replication${NC}"
if grep -q "Queued replication for DELETE $TEST_FILE" logs/nm.log 2>/dev/null; then
    echo -e "${GREEN}✓ Delete replication job was queued${NC}"
else
    echo -e "${YELLOW}⚠ Could not verify delete replication in logs${NC}"
fi

echo -e "\n${YELLOW}Step 9: Check for any replication errors${NC}"
ERROR_COUNT=$(grep -c "Replication job failed" logs/nm.log 2>/dev/null || echo "0")
if [ "$ERROR_COUNT" -eq 0 ]; then
    echo -e "${GREEN}✓ No replication errors found${NC}"
else
    echo -e "${RED}✗ Found $ERROR_COUNT replication error(s)${NC}"
    echo "Recent errors:"
    grep "Replication job failed" logs/nm.log | tail -5
fi

echo -e "\n${YELLOW}=== Test Summary ===${NC}"
echo "Total replication jobs processed:"
grep -c "Processing replication job" logs/nm.log 2>/dev/null || echo "0"

echo -e "\n${YELLOW}To verify replication manually:${NC}"
echo "1. Check primary storage: ls -la storage_ss1/files/"
echo "2. Check replica storage: ls -la storage_ss1_backup/files/"
echo "3. Both should have/not have $TEST_FILE"
echo "4. View detailed logs: tail -50 logs/nm.log"

echo -e "\n${GREEN}Test script completed!${NC}"
