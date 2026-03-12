#!/bin/bash
# Quick script to stop all running NM/SS instances

echo "Stopping all Name Server and Storage Server instances..."
pkill -f bin_nm 2>/dev/null && echo "✓ Stopped NM" || echo "  (No NM running)"
pkill -f bin_ss 2>/dev/null && echo "✓ Stopped SS" || echo "  (No SS running)"
sleep 0.5
echo "Done."

