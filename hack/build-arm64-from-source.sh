#!/usr/bin/env bash
# build-arm64-from-source.sh
#
# Builds a complete Talos Linux arm64 OS image entirely from source,
# without requiring ghcr.io/siderolabs/tools or any pre-built package images.
#
# What is built from source:
#   - Linux kernel 6.12      (cross-compiled with gcc-aarch64-linux-gnu)
#   - containerd v2.2.1      (built from Go source, no network at build time)
#   - runc v1.2.5            (built from Go+CGO source)
#   - init, machined, apid, installer, trustd, maintenance, storaged, dashboard
#     (all Talos Go binaries from this repository)
#
# Output:
#   _out/talos-arm64-from-source/vmlinuz-arm64    - ARM64 Linux kernel
#   _out/talos-arm64-from-source/initramfs-arm64.xz - compressed initramfs (cpio.xz)
#
# Requirements (installed automatically if missing):
#   - gcc-aarch64-linux-gnu, binutils-aarch64-linux-gnu, bc, flex, bison,
#     libssl-dev, libelf-dev, cpio, xz-utils
#   - Go 1.26.0 (built from source if not available; needs Go >= 1.22.6 as bootstrap)
#   - curl, git, xz, cpio
#
# Usage:
#   sudo ./hack/build-arm64-from-source.sh
#
# Note: kernel build takes ~30 min on 16 CPUs. Total ~45-60 min.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/_out"
BUILD_DIR="/build"
JOBS="$(nproc)"

KERNEL_VERSION="6.12"
CONTAINERD_VERSION="v2.2.1"
RUNC_VERSION="v1.2.5"
REQUIRED_GO_VERSION="1.26.0"

TALOS_KERNEL_CONFIG_URL="https://raw.githubusercontent.com/siderolabs/pkgs/main/kernel/build/config-arm64"

echo "==> Building Talos Linux arm64 from source"
echo "    Kernel:     Linux ${KERNEL_VERSION}"
echo "    containerd: ${CONTAINERD_VERSION}"
echo "    runc:       ${RUNC_VERSION}"
echo "    Jobs:       ${JOBS}"
echo "    Output:     ${OUT_DIR}/talos-arm64-from-source/"
echo ""

mkdir -p "${OUT_DIR}" "${BUILD_DIR}"

# ─── Step 1: Install build dependencies ─────────────────────────────────────
echo "==> [1/7] Installing build dependencies..."
apt-get install -y -q \
  gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
  bc flex bison libssl-dev libelf-dev cpio xz-utils 2>&1 | tail -3

# ─── Step 2: Ensure Go 1.26.0 ────────────────────────────────────────────────
echo "==> [2/7] Checking Go ${REQUIRED_GO_VERSION} toolchain..."

find_go() {
    local required="${1}"
    for candidate in \
        "/usr/local/go${required}-src/bin/go" \
        "/usr/local/go${required}/bin/go" \
        "/usr/local/go/bin/go" \
        "$(which go 2>/dev/null || true)"; do
        if [[ -x "${candidate}" ]]; then
            local ver
            ver="$(GOTOOLCHAIN=local "${candidate}" version 2>/dev/null | awk '{print $3}' | sed 's/go//')"
            if [[ "$(printf '%s\n%s' "${required}" "${ver}" | sort -V | head -1)" == "${required}" ]]; then
                echo "${candidate}"; return 0
            fi
        fi
    done
    return 1
}

GO_BIN=""
if ! GO_BIN="$(find_go "${REQUIRED_GO_VERSION}")"; then
    echo "    Go ${REQUIRED_GO_VERSION} not found. Building from source..."
    BOOTSTRAP_GO=""
    for b in /usr/local/go1.25.*/bin/go /usr/local/go1.24.*/bin/go /usr/local/go1.23.*/bin/go; do
        [[ -x "${b}" ]] && BOOTSTRAP_GO="${b%/bin/go}" && break
    done
    [[ -z "${BOOTSTRAP_GO}" ]] && { echo "ERROR: need bootstrap Go >= 1.22.6" >&2; exit 1; }
    GO_SRC="/usr/local/go${REQUIRED_GO_VERSION}-src"
    if [[ ! -f "${GO_SRC}/VERSION" ]]; then
        TMP="$(mktemp /tmp/go_src_XXXXXX.tar.gz)"
        curl -fsSL "https://github.com/golang/go/archive/refs/tags/go${REQUIRED_GO_VERSION}.tar.gz" -o "${TMP}"
        mkdir -p "${GO_SRC}" && tar -xzf "${TMP}" -C "${GO_SRC}" --strip-components=1 && rm -f "${TMP}"
    fi
    GOROOT_BOOTSTRAP="${BOOTSTRAP_GO}" "${GO_SRC}/src/make.bash" 2>&1 | tail -3
    GO_BIN="${GO_SRC}/bin/go"
fi
GOROOT="$(dirname "$(dirname "${GO_BIN}")")"
echo "    Using: $(GOTOOLCHAIN=local "${GO_BIN}" version)"

# ─── Step 3: Download module cache for Talos ────────────────────────────────
echo "==> [3/7] Downloading Go modules (Talos + containerd + runc)..."
(cd "${PROJECT_ROOT}"
 GOROOT="${GOROOT}" GOWORK=off CGO_ENABLED=0 GOPROXY=direct GONOSUMDB="*" \
 "${GO_BIN}" mod download 2>&1 | grep -c "^go: downloading" | xargs -I{} echo "    {} modules downloaded" || true)

# ─── Step 4: Build Linux kernel for arm64 ───────────────────────────────────
KERNEL_IMAGE="${BUILD_DIR}/arch/arm64/boot/Image"
if [[ -f "${KERNEL_IMAGE}" ]]; then
    echo "==> [4/7] Kernel already built, skipping."
else
    echo "==> [4/7] Building Linux kernel ${KERNEL_VERSION} for arm64..."
    if [[ ! -f "${BUILD_DIR}/Makefile" ]]; then
        echo "    Downloading kernel source (~230MB)..."
        TMP="$(mktemp /tmp/linux_XXXXXX.tar.gz)"
        curl -fsSL "https://github.com/torvalds/linux/archive/refs/tags/v${KERNEL_VERSION}.tar.gz" -o "${TMP}"
        tar -xzf "${TMP}" -C "${BUILD_DIR}" --strip-components=1
        rm -f "${TMP}"
    fi
    echo "    Fetching Talos kernel config..."
    curl -fsSL "${TALOS_KERNEL_CONFIG_URL}" -o "${BUILD_DIR}/.config"
    echo "    Running olddefconfig..."
    make -C "${BUILD_DIR}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig 2>&1 | tail -3
    echo "    Compiling (${JOBS} jobs, ~30 min)..."
    make -C "${BUILD_DIR}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image -j"${JOBS}" 2>&1 | tail -5
    echo "    Kernel built: $(ls -lh "${KERNEL_IMAGE}" | awk '{print $5}')"
fi

# ─── Step 5: Build all Talos OS Go binaries ──────────────────────────────────
echo "==> [5/7] Building Talos OS binaries for arm64..."
declare -A COMPONENTS=(
    ["machined-linux-arm64"]="./internal/app/machined"
    ["init-linux-arm64"]="./internal/app/init"
    ["apid-linux-arm64"]="./internal/app/apid"
    ["installer-linux-arm64"]="./cmd/installer"
    ["trustd-linux-arm64"]="./internal/app/trustd"
    ["maintenance-linux-arm64"]="./internal/app/maintenance"
    ["storaged-linux-arm64"]="./internal/app/storaged"
    ["dashboard-linux-arm64"]="./internal/app/dashboard"
    ["talosctl-linux-arm64"]="./cmd/talosctl"
)
for binary in "${!COMPONENTS[@]}"; do
    printf "    %-34s" "${binary}..."
    (cd "${PROJECT_ROOT}"
     GOROOT="${GOROOT}" GOWORK=off GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOPROXY=off GONOSUMDB="*" \
     "${GO_BIN}" build -tags "grpcnotrace" -ldflags "-s -w" \
       -o "${OUT_DIR}/${binary}" "${COMPONENTS[$binary]}")
    echo "OK ($(ls -sh "${OUT_DIR}/${binary}" | awk '{print $1}'))"
done

# ─── Step 6: Build containerd from source ────────────────────────────────────
echo "==> [6/7] Building containerd ${CONTAINERD_VERSION} and runc ${RUNC_VERSION} from source..."

CONTAINERD_SRC="${BUILD_DIR}/containerd-src"
if [[ ! -d "${CONTAINERD_SRC}" ]]; then
    echo "    Downloading containerd source..."
    TMP="$(mktemp /tmp/containerd_XXXXXX.tar.gz)"
    curl -fsSL "https://github.com/containerd/containerd/archive/refs/tags/${CONTAINERD_VERSION}.tar.gz" -o "${TMP}"
    mkdir -p "${CONTAINERD_SRC}" && tar -xzf "${TMP}" -C "${CONTAINERD_SRC}" --strip-components=1 && rm -f "${TMP}"
fi
printf "    %-34s" "containerd-linux-arm64..."
(cd "${CONTAINERD_SRC}"
 GOROOT="${GOROOT}" GOWORK=off GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOPROXY=off \
 "${GO_BIN}" build -mod=vendor \
   -ldflags "-s -w -X github.com/containerd/containerd/v2/version.Version=${CONTAINERD_VERSION}" \
   -o "${OUT_DIR}/containerd-linux-arm64" ./cmd/containerd)
echo "OK ($(ls -sh "${OUT_DIR}/containerd-linux-arm64" | awk '{print $1}'))"

RUNC_SRC="${BUILD_DIR}/runc-src"
if [[ ! -d "${RUNC_SRC}" ]]; then
    echo "    Downloading runc source..."
    TMP="$(mktemp /tmp/runc_XXXXXX.tar.gz)"
    curl -fsSL "https://github.com/opencontainers/runc/archive/refs/tags/${RUNC_VERSION}.tar.gz" -o "${TMP}"
    mkdir -p "${RUNC_SRC}" && tar -xzf "${TMP}" -C "${RUNC_SRC}" --strip-components=1 && rm -f "${TMP}"
fi
printf "    %-34s" "runc-linux-arm64..."
(cd "${RUNC_SRC}"
 GOROOT="${GOROOT}" GOWORK=off GOOS=linux GOARCH=arm64 \
 CGO_ENABLED=1 CC=aarch64-linux-gnu-gcc GOPROXY=off \
 "${GO_BIN}" build -mod=vendor \
   -tags "cgo netgo osusergo" -buildvcs=false \
   -ldflags "-s -w -linkmode=external -extldflags '-static'" \
   -o "${OUT_DIR}/runc-linux-arm64" .)
echo "OK ($(ls -sh "${OUT_DIR}/runc-linux-arm64" | awk '{print $1}'))"

# ─── Step 7: Assemble initramfs + package outputs ────────────────────────────
echo "==> [7/7] Assembling initramfs..."
INITRAMFS_DIR="${BUILD_DIR}/initramfs"
rm -rf "${INITRAMFS_DIR}"
mkdir -p "${INITRAMFS_DIR}"/{bin,sbin,etc/containerd,etc/cri/conf.d,proc,sys,dev,run,tmp,var/run,var/lib/containerd}

# Populate
cp "${OUT_DIR}/init-linux-arm64"        "${INITRAMFS_DIR}/init"
cp "${OUT_DIR}/machined-linux-arm64"    "${INITRAMFS_DIR}/sbin/machined"
cp "${OUT_DIR}/apid-linux-arm64"        "${INITRAMFS_DIR}/sbin/apid"
cp "${OUT_DIR}/trustd-linux-arm64"      "${INITRAMFS_DIR}/sbin/trustd"
cp "${OUT_DIR}/maintenance-linux-arm64" "${INITRAMFS_DIR}/sbin/maintenance"
cp "${OUT_DIR}/storaged-linux-arm64"    "${INITRAMFS_DIR}/sbin/storaged"
cp "${OUT_DIR}/dashboard-linux-arm64"   "${INITRAMFS_DIR}/sbin/dashboard"
cp "${OUT_DIR}/talosctl-linux-arm64"    "${INITRAMFS_DIR}/bin/talosctl"
cp "${OUT_DIR}/installer-linux-arm64"   "${INITRAMFS_DIR}/sbin/installer"
cp "${OUT_DIR}/containerd-linux-arm64"  "${INITRAMFS_DIR}/sbin/containerd"
cp "${OUT_DIR}/runc-linux-arm64"        "${INITRAMFS_DIR}/sbin/runc"
chmod +x "${INITRAMFS_DIR}/init" "${INITRAMFS_DIR}/sbin/"* "${INITRAMFS_DIR}/bin/"*

# OS identity file
cat > "${INITRAMFS_DIR}/etc/os-release" << 'OSEOF'
NAME="Talos"
ID=talos
VERSION_ID=v1.13.0-alpha.1
PRETTY_NAME="Talos v1.13.0-alpha.1 (built from source)"
HOME_URL="https://www.talos.dev/"
OSEOF

# containerd config
cat > "${INITRAMFS_DIR}/etc/containerd/config.toml" << 'CTDEOF'
version = 3
[plugins]
  [plugins.'io.containerd.cri.v1.runtime']
    [plugins.'io.containerd.cri.v1.runtime'.containerd]
      [plugins.'io.containerd.cri.v1.runtime'.containerd.runtimes]
        [plugins.'io.containerd.cri.v1.runtime'.containerd.runtimes.runc]
          runtime_type = 'io.containerd.runc.v2'
CTDEOF

# Pack as cpio.xz
echo "    Packing initramfs..."
FINAL="${OUT_DIR}/talos-arm64-from-source"
mkdir -p "${FINAL}"
(cd "${INITRAMFS_DIR}" && find . | cpio -o -H newc 2>/dev/null | xz -T0 -9 > "${FINAL}/initramfs-arm64.xz")
cp "${KERNEL_IMAGE}" "${FINAL}/vmlinuz-arm64"

echo ""
echo "==> BUILD COMPLETE"
echo ""
echo "Output: ${FINAL}/"
ls -lh "${FINAL}/"
echo ""
echo "Components:"
echo "  vmlinuz-arm64     : Linux ${KERNEL_VERSION} (cross-compiled from source)"
echo "  initramfs-arm64.xz: cpio.xz containing:"
echo "    - init/machined/apid/trustd/storaged/maintenance/dashboard (Talos)"
echo "    - installer/talosctl"
echo "    - containerd ${CONTAINERD_VERSION} (built from source)"
echo "    - runc ${RUNC_VERSION} (built from source)"
echo ""
echo "Boot with QEMU (for testing):"
echo "  qemu-system-aarch64 -M virt -cpu cortex-a57 -m 2G \\"
echo "    -kernel ${FINAL}/vmlinuz-arm64 \\"
echo "    -initrd ${FINAL}/initramfs-arm64.xz \\"
echo "    -append 'console=ttyAMA0 panic=10 talos.board=generic' \\"
echo "    -nographic"
