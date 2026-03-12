#!/bin/bash
# Script to run all three components (NM, SS, Client) for Phase 1 testing

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    pkill -f bin_nm 2>/dev/null || true
    pkill -f bin_ss 2>/dev/null || true
    sleep 0.5
    echo -e "${GREEN}Done.${NC}"
}

# Trap Ctrl+C
trap cleanup EXIT INT TERM

# Kill any existing instances
echo -e "${YELLOW}Killing any existing instances...${NC}"
pkill -f bin_nm 2>/dev/null || true
pkill -f bin_ss 2>/dev/null || true
sleep 1

# Check if binaries exist
if [ ! -f "./bin_nm" ] || [ ! -f "./bin_ss" ] || [ ! -f "./bin_client" ]; then
    echo -e "${RED}Error: Binaries not found. Run 'make' first.${NC}"
    exit 1
fi

# Configuration
NM_HOST="127.0.0.1"
NM_PORT="5000"
SS_HOST="127.0.0.1"
SS_CLIENT_PORT="6001"
SS_STORAGE="./storage_ss1"
SS_USERNAME="ss1"
CLIENT_USERNAME="alice"

echo -e "${GREEN}Starting Name Server (NM)...${NC}"
./bin_nm --host "$NM_HOST" --port "$NM_PORT" > nm.log 2>&1 &
NM_PID=$!
sleep 1

if ! kill -0 $NM_PID 2>/dev/null; then
    echo -e "${RED}Error: NM failed to start. Check nm.log${NC}"
    exit 1
fi
echo -e "${GREEN}NM started (PID: $NM_PID)${NC}"

echo -e "${GREEN}Starting Storage Server (SS)...${NC}"
./bin_ss --nm-host "$NM_HOST" --nm-port "$NM_PORT" \
         --host "$SS_HOST" --client-port "$SS_CLIENT_PORT" \
         --storage "$SS_STORAGE" --username "$SS_USERNAME" > ss.log 2>&1 &
SS_PID=$!
sleep 1

if ! kill -0 $SS_PID 2>/dev/null; then
    echo -e "${RED}Error: SS failed to start. Check ss.log${NC}"
    cleanup
    exit 1
fi
echo -e "${GREEN}SS started (PID: $SS_PID)${NC}"

echo -e "\n${GREEN}All servers running!${NC}"
echo -e "${YELLOW}NM logs: tail -f nm.log${NC}"
echo -e "${YELLOW}SS logs: tail -f ss.log${NC}"
echo -e "\n${YELLOW}Running client registration...${NC}"
sleep 1

./bin_client --nm-host "$NM_HOST" --nm-port "$NM_PORT" --username "$CLIENT_USERNAME"

echo -e "\n${GREEN}Client registration complete.${NC}"
echo -e "\n${YELLOW}Press Ctrl+C to stop all servers.${NC}"
echo -e "${YELLOW}Or run './bin_client' again manually.${NC}"

# Keep running until interrupted
wait

