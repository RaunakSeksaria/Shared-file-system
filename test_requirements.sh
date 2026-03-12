#!/bin/bash
# Comprehensive test script for checking requirements
# Tests all user functionalities and system requirements

set -e  # Exit on error for critical setup steps

echo "=========================================="
echo "  REQUIREMENTS TESTING SCRIPT"
echo "=========================================="
echo ""

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test results tracking
PASSED=0
FAILED=0
TOTAL=0

# Function to print test results
print_test() {
    local test_name=$1
    local result=$2
    TOTAL=$((TOTAL + 1))
    
    if [ "$result" = "PASS" ]; then
        echo -e "${GREEN}✓${NC} [$TOTAL] $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗${NC} [$TOTAL] $test_name"
        FAILED=$((FAILED + 1))
    fi
}

# Function to run command and check output
run_test() {
    local test_name=$1
    local command=$2
    local expected_pattern=$3
    
    echo -e "\n${BLUE}Testing:${NC} $test_name"
    echo "Command: $command"
    
    output=$(eval "$command" 2>&1)
    echo "Output: $output"
    
    if echo "$output" | grep -qi "$expected_pattern"; then
        print_test "$test_name" "PASS"
        return 0
    else
        print_test "$test_name" "FAIL"
        echo "  Expected pattern: $expected_pattern"
        return 1
    fi
}

# Helper function to run client command
run_client_cmd() {
    local cmd=$1
    local username=$2
    echo -e "$cmd\nEXIT" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username "$username" 2>&1 | grep -v "LangOS Client" | grep -v "^>" | grep -v "Exiting"
}

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    pkill -9 bin_nm bin_ss bin_client 2>/dev/null || true
    sleep 1
}

# Setup function
setup() {
    echo -e "${YELLOW}Setting up test environment...${NC}"
    
    # Clean up any running processes
    cleanup
    
    # Start NM
    echo "Starting Name Server..."
    ./bin_nm > /dev/null 2>&1 &
    NM_PID=$!
    sleep 2
    
    if ! ps -p $NM_PID > /dev/null; then
        echo -e "${RED}Failed to start Name Server${NC}"
        exit 1
    fi
    echo "Name Server started (PID: $NM_PID)"
    
    # Start Storage Servers
    echo "Starting Storage Servers..."
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9001 --storage storage_ss1 --username ss1 > /dev/null 2>&1 &
    SS1_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9011 --storage storage_ss2 --username ss2 > /dev/null 2>&1 &
    SS2_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9021 --storage storage_ss3 --username ss3 > /dev/null 2>&1 &
    SS3_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9031 --storage storage_ss4 --username ss4 > /dev/null 2>&1 &
    SS4_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9041 --storage storage_ss5 --username ss5 > /dev/null 2>&1 &
    SS5_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9051 --storage storage_ss6 --username ss6 > /dev/null 2>&1 &
    SS6_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9061 --storage storage_ss7 --username ss7 > /dev/null 2>&1 &
    SS7_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9003 --storage storage_ss1_backup --username ss1_backup > /dev/null 2>&1 &
    SS1_BACKUP_PID=$!
    sleep 1
    
    ./bin_ss --nm-host 127.0.0.1 --nm-port 5000 --host 127.0.0.1 --client-port 9053 --storage storage_ss6_backup --username ss6_backup > /dev/null 2>&1 &
    SS6_BACKUP_PID=$!
    sleep 2
    
    echo "Storage Servers started (7 primary + 2 backups)"
    echo ""
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

# Start testing
echo -e "${BLUE}Starting requirement tests...${NC}\n"

setup

echo "=========================================="
echo "  USER FUNCTIONALITIES (150 marks)"
echo "=========================================="

# [10] View Files
echo -e "\n${YELLOW}=== Test 1: VIEW command (10 marks) ===${NC}"
run_test "VIEW - List accessible files" "run_client_cmd 'VIEW' raju" "bablu.txt\|file2.txt\|\.txt\|-->"
run_test "VIEW -a - List all files" "run_client_cmd 'VIEW -a' raju" "bablu.txt\|file2.txt\|-->"
run_test "VIEW -l - List with details" "run_client_cmd 'VIEW -l' raju" "Filename\|Words\|Chars"
run_test "VIEW -al - List all with details" "run_client_cmd 'VIEW -al' raju" "Filename\|Words\|Chars"

# [10] Read File
echo -e "\n${YELLOW}=== Test 2: READ command (10 marks) ===${NC}"
if [ -f "storage_ss1/files/bablu.txt" ]; then
    run_test "READ - Read file content" "run_client_cmd 'READ bablu.txt' raju" "."
else
    echo "Creating test file for READ test..."
    run_client_cmd "CREATE testread.txt" raju
    echo -e "1 hello world\nETIRW" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju
    run_test "READ - Read file content" "run_client_cmd 'READ testread.txt' raju" "hello\|world"
fi

# [10] Create File
echo -e "\n${YELLOW}=== Test 3: CREATE command (10 marks) ===${NC}"
TEST_CREATE_FILE="testfile_$(date +%s).txt"
run_test "CREATE - Create new file" "run_client_cmd 'CREATE $TEST_CREATE_FILE' raju" "success\|created\|Created"

# [30] Write to File
echo -e "\n${YELLOW}=== Test 4: WRITE command (30 marks) ===${NC}"
TEST_FILE="writetest_$(date +%s).txt"
run_client_cmd "CREATE $TEST_FILE" raju > /dev/null
sleep 1

# Test basic write
run_test "WRITE - Basic write to file" "echo -e 'WRITE $TEST_FILE\n1 Hello world.\nETIRW\nEXIT' | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju 2>&1 | grep -v 'LangOS' | grep -v '^>' | grep -v 'Exiting'" "success\|Success\|written"

# Test sentence insertion
run_test "WRITE - Insert into sentence" "echo -e 'WRITE $TEST_FILE\n2 beautiful\nETIRW\nEXIT' | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju 2>&1 | grep -v 'LangOS' | grep -v '^>' | grep -v 'Exiting'" "success\|Success"

# Verify content
run_test "READ after WRITE - Verify content" "run_client_cmd 'READ $TEST_FILE' raju" "Hello\|beautiful"

# [15] Undo Change
echo -e "\n${YELLOW}=== Test 5: UNDO command (15 marks) ===${NC}"
run_test "UNDO - Revert last change" "run_client_cmd 'UNDO $TEST_FILE' raju" "success\|reverted\|Undo"

# [10] Get Info
echo -e "\n${YELLOW}=== Test 6: INFO command (10 marks) ===${NC}"
run_test "INFO - Get file metadata" "run_client_cmd 'INFO $TEST_FILE' raju" "File\|Owner\|Size\|Access"

# [10] Delete File
echo -e "\n${YELLOW}=== Test 7: DELETE command (10 marks) ===${NC}"
DELETE_FILE="deleteme_$(date +%s).txt"
run_client_cmd "CREATE $DELETE_FILE" raju > /dev/null
sleep 1
run_test "DELETE - Remove file" "run_client_cmd 'DELETE $DELETE_FILE' raju" "success\|deleted\|Deleted"

# [15] Stream Content
echo -e "\n${YELLOW}=== Test 8: STREAM command (15 marks) ===${NC}"
if [ -f "storage_ss1/files/bablu.txt" ]; then
    run_test "STREAM - Stream file content" "timeout 5 run_client_cmd 'STREAM bablu.txt' raju" "."
else
    run_test "STREAM - Stream file content" "timeout 5 run_client_cmd 'STREAM $TEST_FILE' raju" "."
fi

# [10] List Users
echo -e "\n${YELLOW}=== Test 9: LIST command (10 marks) ===${NC}"
run_test "LIST - List all users" "run_client_cmd 'LIST' raju" "raju\|user"

# [15] Access Control
echo -e "\n${YELLOW}=== Test 10: ACCESS commands (15 marks) ===${NC}"
ACCESS_FILE="accesstest_$(date +%s).txt"
run_client_cmd "CREATE $ACCESS_FILE" raju > /dev/null
sleep 1

run_test "ADDACCESS -R - Grant read access" "run_client_cmd 'ADDACCESS -R $ACCESS_FILE babu' raju" "success\|granted"
run_test "ADDACCESS -W - Grant write access" "run_client_cmd 'ADDACCESS -W $ACCESS_FILE babu' raju" "success\|granted"
run_test "REMACCESS - Remove access" "run_client_cmd 'REMACCESS $ACCESS_FILE babu' raju" "success\|removed"

# [15] Execute File
echo -e "\n${YELLOW}=== Test 11: EXEC command (15 marks) ===${NC}"
EXEC_FILE="exectest_$(date +%s).txt"
run_client_cmd "CREATE $EXEC_FILE" raju > /dev/null
sleep 1
echo -e "WRITE $EXEC_FILE\n1 echo Hello from exec.\nETIRW\nEXIT" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju > /dev/null 2>&1
sleep 1
run_test "EXEC - Execute file content" "run_client_cmd 'EXEC $EXEC_FILE' raju" "Hello from exec"

echo "=========================================="
echo "  SYSTEM REQUIREMENTS (40 marks)"
echo "=========================================="

# [10] Data Persistence
echo -e "\n${YELLOW}=== Test 12: Data Persistence (10 marks) ===${NC}"
PERSIST_FILE="persist_$(date +%s).txt"
run_client_cmd "CREATE $PERSIST_FILE" raju > /dev/null
sleep 1
echo -e "WRITE $PERSIST_FILE\n1 Persistent data test.\nETIRW\nEXIT" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju > /dev/null 2>&1
sleep 1

# Check if file exists on disk
if [ -f "storage_ss1/files/$PERSIST_FILE" ] || [ -f "storage_ss2/files/$PERSIST_FILE" ]; then
    print_test "Data Persistence - File stored on disk" "PASS"
else
    print_test "Data Persistence - File stored on disk" "FAIL"
fi

# Check metadata
if [ -f "storage_ss1/metadata/$PERSIST_FILE.meta" ] || [ -f "storage_ss2/metadata/$PERSIST_FILE.meta" ]; then
    print_test "Data Persistence - Metadata stored" "PASS"
else
    print_test "Data Persistence - Metadata stored" "FAIL"
fi

# [5] Access Control Enforcement
echo -e "\n${YELLOW}=== Test 13: Access Control Enforcement (5 marks) ===${NC}"
AC_FILE="actest_$(date +%s).txt"
run_client_cmd "CREATE $AC_FILE" raju > /dev/null
sleep 1

# Try to read without access (using different user)
output=$(run_client_cmd "READ $AC_FILE" babu 2>&1 || true)
if echo "$output" | grep -qi "denied\|unauthorized\|no access\|permission"; then
    print_test "Access Control - Unauthorized read blocked" "PASS"
else
    echo "Warning: Could not verify access control (output: $output)"
    print_test "Access Control - Unauthorized read blocked" "FAIL"
fi

# [5] Logging
echo -e "\n${YELLOW}=== Test 14: Logging (5 marks) ===${NC}"
if [ -f "logs/nm.log" ] || [ -f "nm.log" ]; then
    if grep -q "request\|REQUEST\|response\|RESPONSE" logs/nm.log 2>/dev/null || grep -q "request\|REQUEST\|response\|RESPONSE" nm.log 2>/dev/null; then
        print_test "Logging - NM logs operations" "PASS"
    else
        print_test "Logging - NM logs operations" "FAIL"
    fi
else
    print_test "Logging - NM logs operations" "FAIL"
fi

# [5] Error Handling
echo -e "\n${YELLOW}=== Test 15: Error Handling (5 marks) ===${NC}"

# Test 1: File not found
output=$(run_client_cmd "READ nonexistent_file_12345.txt" raju 2>&1 || true)
if echo "$output" | grep -qi "error\|not found\|does not exist"; then
    print_test "Error Handling - File not found error" "PASS"
else
    print_test "Error Handling - File not found error" "FAIL"
fi

# Test 2: Invalid sentence index
output=$(echo -e "WRITE $TEST_FILE\n99999 test\nETIRW\nEXIT" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju 2>&1 || true)
if echo "$output" | grep -qi "error\|invalid\|out of range"; then
    print_test "Error Handling - Invalid index error" "PASS"
else
    print_test "Error Handling - Invalid index error" "FAIL"
fi

# [15] Efficient Search
echo -e "\n${YELLOW}=== Test 16: Efficient Search (15 marks) ===${NC}"
# This is hard to test directly, but we can check if VIEW is fast
start_time=$(date +%s%N)
run_client_cmd "VIEW" raju > /dev/null 2>&1
end_time=$(date +%s%N)
elapsed=$((($end_time - $start_time) / 1000000))  # Convert to milliseconds

echo "VIEW command took ${elapsed}ms"
if [ $elapsed -lt 1000 ]; then
    print_test "Efficient Search - VIEW performance" "PASS"
else
    print_test "Efficient Search - VIEW performance (may need optimization)" "FAIL"
fi

echo "=========================================="
echo "  BONUS: FAULT TOLERANCE (15 marks)"
echo "=========================================="

# Test replication and failover
echo -e "\n${YELLOW}=== Test 17: Replication & Failover ===${NC}"

FAILOVER_FILE="failover_$(date +%s).txt"
run_client_cmd "CREATE $FAILOVER_FILE" raju > /dev/null
sleep 1
echo -e "WRITE $FAILOVER_FILE\n1 Test failover content.\nETIRW\nEXIT" | ./bin_client --nm-host 127.0.0.1 --nm-port 5000 --username raju > /dev/null 2>&1
sleep 3  # Wait for replication

# Check if file is replicated
if [ -f "storage_ss1/files/$FAILOVER_FILE" ] && [ -f "storage_ss1_backup/files/$FAILOVER_FILE" ]; then
    print_test "Replication - File replicated to backup" "PASS"
else
    print_test "Replication - File replicated to backup" "FAIL"
fi

# Kill primary and test failover
echo "Testing failover by killing primary..."
pkill -9 -f "bin_ss.*storage_ss1 " 2>/dev/null || true
sleep 20  # Wait for failure detection and failover

# Try to read file from backup
output=$(run_client_cmd "READ $FAILOVER_FILE" raju 2>&1)
if echo "$output" | grep -qi "Test failover content\|failover"; then
    print_test "Failover - Read from backup after primary failure" "PASS"
else
    print_test "Failover - Read from backup after primary failure" "FAIL"
    echo "Output was: $output"
fi

echo ""
echo "=========================================="
echo "  TEST SUMMARY"
echo "=========================================="
echo -e "${GREEN}Passed:${NC} $PASSED / $TOTAL"
echo -e "${RED}Failed:${NC} $FAILED / $TOTAL"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
else
    echo -e "${YELLOW}! Some tests failed. Review the output above.${NC}"
    exit 1
fi
