#!/bin/sh
# Install build/auto-chre.fwsc on an FM-1 over USB (tools/fm1_install.py; see BUILDING.md).
cd "$(dirname "$0")"
python3 tools/fm1_install.py "${1:-build/auto-chre.fwsc}"
