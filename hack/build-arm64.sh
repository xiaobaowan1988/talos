#!/usr/bin/env bash
# build-arm64.sh - Build talosctl for arm64 and run tests
#
# This script builds the talosctl binary for arm64 from an x86_64 host.
# It handles the Go 1.26.0 toolchain requirement by building from source
# when the required Go version is not available.
#
# Usage: ./hack/build-arm64.sh [--test]
#   --test    Also run unit tests after building

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/_out"

# Required Go version (from go.mod)
REQUIRED_GO_VERSION="1.26.0"
GO_BIN=""

echo "==> Building talosctl for arm64"
echo "    Project: ${PROJECT_ROOT}"
echo "    Output:  ${OUT_DIR}"

mkdir -p "${OUT_DIR}"

# Find a suitable Go toolchain
find_go() {
    local required="${1}"
    # Check common installation paths
    for candidate in \
        "/usr/local/go${required}/bin/go" \
        "/usr/local/go${required}-src/bin/go" \
        "/usr/local/go/bin/go" \
        "$(which go 2>/dev/null || true)"; do
        if [[ -x "${candidate}" ]]; then
            local ver
            ver="$(GOTOOLCHAIN=local "${candidate}" version 2>/dev/null | awk '{print $3}' | sed 's/go//')"
            if [[ "$(printf '%s\n%s' "${required}" "${ver}" | sort -V | head -1)" == "${required}" ]]; then
                echo "${candidate}"
                return 0
            fi
        fi
    done
    return 1
}

# Try to find Go >= 1.26.0
echo "==> Looking for Go >= ${REQUIRED_GO_VERSION}..."
if GO_BIN="$(find_go "${REQUIRED_GO_VERSION}")"; then
    echo "    Found: ${GO_BIN} ($(GOTOOLCHAIN=local "${GO_BIN}" version))"
else
    echo "    Not found. Building Go ${REQUIRED_GO_VERSION} from source..."

    # Find bootstrap Go (>= 1.22.6 required to build Go 1.26)
    BOOTSTRAP_GO=""
    for candidate in /usr/local/go1.25.*/bin/go /usr/local/go1.24.*/bin/go /usr/local/go1.23.*/bin/go /usr/local/go1.22.*/bin/go; do
        if [[ -x "${candidate}" ]]; then
            BOOTSTRAP_GO="${candidate%/bin/go}"
            echo "    Using bootstrap: ${BOOTSTRAP_GO}"
            break
        fi
    done

    if [[ -z "${BOOTSTRAP_GO}" ]]; then
        echo "ERROR: No suitable bootstrap Go compiler found (need >= 1.22.6)" >&2
        exit 1
    fi

    # Download Go source from GitHub
    GO_SRC_DIR="/usr/local/go${REQUIRED_GO_VERSION}-src"
    if [[ ! -f "${GO_SRC_DIR}/VERSION" ]]; then
        echo "    Downloading Go ${REQUIRED_GO_VERSION} source from GitHub..."
        TMP_TAR="$(mktemp /tmp/go_src_XXXXXX.tar.gz)"
        curl -fsSL "https://github.com/golang/go/archive/refs/tags/go${REQUIRED_GO_VERSION}.tar.gz" -o "${TMP_TAR}"
        mkdir -p "${GO_SRC_DIR}"
        tar -xzf "${TMP_TAR}" -C "${GO_SRC_DIR}" --strip-components=1
        rm -f "${TMP_TAR}"
    fi

    echo "    Building Go ${REQUIRED_GO_VERSION}..."
    GOROOT_BOOTSTRAP="${BOOTSTRAP_GO}" "${GO_SRC_DIR}/src/make.bash" 2>&1 | tail -5
    GO_BIN="${GO_SRC_DIR}/bin/go"
    echo "    Built: $(GOTOOLCHAIN=local "${GO_BIN}" version)"
fi

# Download modules if not cached
echo "==> Downloading Go modules..."
GOROOT="$(dirname "$(dirname "${GO_BIN}")")"
(
    cd "${PROJECT_ROOT}"
    GOROOT="${GOROOT}" \
    GOWORK=off \
    CGO_ENABLED=0 \
    GOPROXY=direct \
    GONOSUMDB="*" \
    "${GO_BIN}" mod download 2>&1 | grep -E "^go: (downloading|error)" || true
)

# Build talosctl for arm64
echo "==> Cross-compiling talosctl for linux/arm64..."
GOROOT="$(dirname "$(dirname "${GO_BIN}")")"
(
    cd "${PROJECT_ROOT}"
    GOROOT="${GOROOT}" \
    GOWORK=off \
    GOOS=linux \
    GOARCH=arm64 \
    CGO_ENABLED=0 \
    GOPROXY=off \
    GONOSUMDB="*" \
    "${GO_BIN}" build \
        -tags "grpcnotrace" \
        -ldflags "-s -w" \
        -o "${OUT_DIR}/talosctl-linux-arm64" \
        ./cmd/talosctl
)

echo "==> Build successful!"
ls -lh "${OUT_DIR}/talosctl-linux-arm64"
file "${OUT_DIR}/talosctl-linux-arm64"

# Test the binary using QEMU if available
if command -v qemu-aarch64-static &>/dev/null; then
    echo ""
    echo "==> Testing arm64 binary with QEMU..."
    qemu-aarch64-static "${OUT_DIR}/talosctl-linux-arm64" version --short 2>&1 || true
else
    echo ""
    echo "NOTE: qemu-aarch64-static not found. To test the arm64 binary:"
    echo "      apt-get install -y qemu-user-static"
    echo "      qemu-aarch64-static ${OUT_DIR}/talosctl-linux-arm64 version --short"
fi

# Run unit tests if requested
if [[ "${1:-}" == "--test" ]]; then
    echo ""
    echo "==> Running unit tests..."
    GOROOT="$(dirname "$(dirname "${GO_BIN}")")"
    (
        cd "${PROJECT_ROOT}"
        GOROOT="${GOROOT}" \
        GOWORK=off \
        CGO_ENABLED=0 \
        GOPROXY=off \
        GONOSUMDB="*" \
        "${GO_BIN}" test \
            -tags "grpcnotrace" \
            -count=1 \
            -timeout 120s \
            ./pkg/argsbuilder/... \
            ./pkg/bytesize/... \
            ./pkg/chunker/... \
            ./pkg/conditions/... \
            ./pkg/grpc/... \
            ./internal/pkg/secureboot/... \
            ./internal/pkg/toml/... \
            2>&1
    )
fi

echo ""
echo "==> Done!"
