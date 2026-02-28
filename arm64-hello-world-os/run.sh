#!/bin/bash
# run.sh - Build and run the ARM64 Hello World OS in QEMU
#
# Usage:
#   ./run.sh          Build and run
#   ./run.sh build    Build only
#   ./run.sh clean    Clean build artifacts
#   ./run.sh debug    Build and run with GDB server

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Check for required tools
check_tool() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: $1 is not installed."
        echo ""
        echo "Install on Ubuntu/Debian:"
        echo "  sudo apt-get install $2"
        echo ""
        echo "Install on Fedora:"
        echo "  sudo dnf install $3"
        echo ""
        echo "Install on macOS (with Homebrew):"
        echo "  brew install $4"
        exit 1
    fi
}

check_tool aarch64-linux-gnu-gcc "gcc-aarch64-linux-gnu" "gcc-aarch64-linux-gnu" "aarch64-elf-gcc"
check_tool qemu-system-aarch64 "qemu-system-arm" "qemu-system-aarch64" "qemu"

case "${1:-run}" in
    build)
        echo "==> Building ARM64 Hello World OS..."
        make clean
        make
        echo "==> Build complete: kernel.img"
        ;;
    clean)
        echo "==> Cleaning..."
        make clean
        echo "==> Done."
        ;;
    debug)
        echo "==> Building and running with GDB server..."
        echo "    Connect with: gdb-multiarch kernel.elf -ex 'target remote :1234'"
        make
        make run-debug
        ;;
    run|"")
        echo "==> Building ARM64 Hello World OS..."
        make
        echo ""
        echo "==> Running in QEMU (press Ctrl-A then X to exit)..."
        echo ""
        make run
        ;;
    *)
        echo "Usage: $0 [build|run|clean|debug]"
        exit 1
        ;;
esac
