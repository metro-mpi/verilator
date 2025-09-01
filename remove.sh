#!/bin/bash
set -e

# === CONFIGURATION ===
VERILATOR_VERSION="v5.036"
INSTALL_PREFIX="/usr/local"  # Change to $HOME/verilator if you want a local install
SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"
NUM_CORES=$(nproc)

# === CHECK IF VERILATOR IS INSTALLED ===
if command -v verilator &> /dev/null; then
    echo "Verilator is already installed at: $(which verilator)"
    echo "Removing existing Verilator..."

    # Remove manually installed verilator if in /usr/local/bin
    if [[ -f "$INSTALL_PREFIX/bin/verilator" ]]; then
        sudo rm -f "$INSTALL_PREFIX/bin/verilator"
        echo "Verilator Removed!!!"
    fi
fi
