#!/usr/bin/env bash
# Build open5gs-nwdafd: dependencies, configure, build, tests and install.
#
#   ./build.sh                       the Rel-18 compliant build (HTTP/2 + TLS)
#   ./build.sh --deps --tests        install the apt packages first, then run the tests
#   ./build.sh --profile full        HTTP/2 without TLS (OpenSSL < 3.0, e.g. Ubuntu 20.04)
#   ./build.sh --install             then install (binary, config, OpenAPI files, systemd unit)
#
# The first configure downloads the C++ dependencies and the official 3GPP
# OpenAPI files, so it needs network access (or --openapi-dir for the latter).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PROFILE=rel18
BUILD_DIR="$ROOT/build"
BUILD_TYPE=Release
JOBS="$(nproc 2>/dev/null || echo 2)"
DEPS=0 TESTS=0 INSTALL=0 CLEAN=0
OPENAPI_DIR=""

usage() {
    cat <<'EOF'
usage: ./build.sh [options]
  --profile P        rel18 (default): HTTP/2 + TLS, the profile the Rel-18 claim covers
                     full:    HTTP/2, TLS off (for OpenSSL < 3.0, such as Ubuntu 20.04)
                     minimal: no journald, TLS or HTTP/2 (dev-legacy: can't register with
                              an Open5GS NRF, which needs HTTP/2)
  --deps             install the apt packages the profile needs (uses sudo when not root)
  --tests            build and run the tests
  --install          install after building (cmake --install, uses sudo when not root)
  --clean            remove the build directory first
  --build-dir DIR    build directory (default: build/)
  --jobs N           parallel jobs (default: all cores)
  --openapi-dir DIR  an offline copy of the official 3GPP OpenAPI files (still hash-checked)
  --debug            Debug instead of Release
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --profile)     PROFILE=$2; shift 2 ;;
        --deps)        DEPS=1; shift ;;
        --tests)       TESTS=1; shift ;;
        --install)     INSTALL=1; shift ;;
        --clean)       CLEAN=1; shift ;;
        --build-dir)   BUILD_DIR=$2; shift 2 ;;
        --jobs)        JOBS=$2; shift 2 ;;
        --openapi-dir) OPENAPI_DIR="$(cd "$2" && pwd)"; shift 2 ;;
        --debug)       BUILD_TYPE=Debug; shift ;;
        -h|--help)     usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

say()  { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
SUDO=""; [[ $(id -u) -ne 0 ]] && SUDO=sudo

# ── Profile → CMake flags and apt packages ──────────────────────────────────
PKGS=(build-essential cmake git pkg-config ca-certificates libsqlite3-dev curl jq)   # curl, jq: nwdaf-cli
case "$PROFILE" in
    rel18)
        FLAGS=(-DNWDAF_USE_SD_JOURNAL=ON -DNWDAF_USE_TLS=ON -DNWDAF_USE_HTTP2=ON -DNWDAF_REQUIRE_REL18_PROFILE=ON)
        PKGS+=(libsystemd-dev libssl-dev libnghttp2-dev libcurl4-openssl-dev) ;;
    full)
        FLAGS=(-DNWDAF_USE_SD_JOURNAL=ON -DNWDAF_USE_TLS=OFF -DNWDAF_USE_HTTP2=ON)
        PKGS+=(libsystemd-dev libnghttp2-dev libcurl4-openssl-dev) ;;
    minimal)
        FLAGS=(-DNWDAF_USE_SD_JOURNAL=OFF -DNWDAF_USE_TLS=OFF -DNWDAF_USE_HTTP2=OFF) ;;
    *) die "unknown profile '$PROFILE' (rel18, full or minimal)" ;;
esac
FLAGS+=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DNWDAF_BUILD_TESTS=$([[ $TESTS == 1 ]] && echo ON || echo OFF))
[[ -n "$OPENAPI_DIR" ]] && FLAGS+=(-DNWDAF_3GPP_OPENAPI_SOURCE_DIR="$OPENAPI_DIR")

if [[ $DEPS == 1 ]]; then
    command -v apt-get >/dev/null || die "--deps needs apt-get; install these yourself: ${PKGS[*]}"
    say "Installing packages: ${PKGS[*]}"
    $SUDO apt-get update -qq
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y --no-install-recommends "${PKGS[@]}"
fi

# ── Toolchain checks ────────────────────────────────────────────────────────
command -v cmake >/dev/null || die "cmake not found (run with --deps, or install CMake >= 3.22)"
CMAKE_VERSION="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?')"
CMAKE_MAJOR=${CMAKE_VERSION%%.*}
CMAKE_MINOR=$(cut -d. -f2 <<<"$CMAKE_VERSION")
if (( CMAKE_MAJOR < 3 || (CMAKE_MAJOR == 3 && CMAKE_MINOR < 22) )); then
    die "CMake $CMAKE_VERSION is too old (>= 3.22 needed); e.g. pip3 install 'cmake>=3.22,<4'"
fi
# CMake 4 dropped compatibility with fetched dependencies' cmake_minimum_required.
(( CMAKE_MAJOR >= 4 )) && FLAGS+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)

CXX_BIN="${CXX:-g++}"
command -v "$CXX_BIN" >/dev/null || die "no C++ compiler ($CXX_BIN); run with --deps or install build-essential"
if "$CXX_BIN" --version | head -1 | grep -q "g++\|GCC"; then
    GCC_MAJOR="$("$CXX_BIN" -dumpversion | cut -d. -f1)"
    (( GCC_MAJOR >= 16 )) && warn "GCC $GCC_MAJOR: the pinned yaml-cpp 0.8.0 doesn't build with GCC >= 16 (missing <cstdint>); use GCC <= 15 (CXX=g++-15)"
fi

if [[ $PROFILE == rel18 ]]; then
    SSL="$(pkg-config --modversion openssl 2>/dev/null || true)"
    [[ -z "$SSL" ]] && die "the rel18 profile needs OpenSSL >= 3.0 headers (libssl-dev); run with --deps"
    [[ ${SSL%%.*} -lt 3 ]] && die "OpenSSL $SSL is older than 3.0, which TLS needs: use --profile full (TLS off)"
fi

# ── Configure, build, test, install ─────────────────────────────────────────
[[ $CLEAN == 1 ]] && { say "Removing $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

say "Configuring ($PROFILE, $BUILD_TYPE) in $BUILD_DIR"
# Retried: the first configure downloads dependencies and the 3GPP OpenAPI files.
for attempt in 1 2 3; do
    if cmake -S "$ROOT" -B "$BUILD_DIR" "${FLAGS[@]}"; then break; fi
    [[ $attempt == 3 ]] && die "configure failed (see above); for the 3GPP files offline, use --openapi-dir"
    warn "configure attempt $attempt failed (often a download); retrying"
    sleep $((attempt * 10))
done

say "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [[ $TESTS == 1 ]]; then
    say "Running the tests"
    (cd "$BUILD_DIR" && ctest --output-on-failure --timeout 120)
fi

if [[ $INSTALL == 1 ]]; then
    say "Installing"
    $SUDO cmake --install "$BUILD_DIR"
    # The systemd unit runs as `open5gs` (the Open5GS packages' user), may
    # write only to /opt/nwdaf and /var/log/open5gs, and reads the NFs'
    # journals through the systemd-journal group.
    if id open5gs >/dev/null 2>&1; then
        say "Preparing /opt/nwdaf and the open5gs user for the service"
        $SUDO mkdir -p /opt/nwdaf/models /var/log/open5gs
        $SUDO chown -R open5gs:open5gs /opt/nwdaf
        if getent group systemd-journal >/dev/null; then $SUDO usermod -aG systemd-journal open5gs; fi
        if command -v systemctl >/dev/null; then $SUDO systemctl daemon-reload || true; fi
    else
        warn "no 'open5gs' user (Open5GS isn't installed here): create it, or change User= in /etc/systemd/system/open5gs-nwdafd.service"
    fi
fi

say "Done: $BUILD_DIR/open5gs-nwdafd ($PROFILE profile)"
if [[ $INSTALL == 1 ]]; then
    echo "    Configure /etc/open5gs/nwdaf.yaml, then: sudo systemctl enable --now open5gs-nwdafd"
else
    echo "    Install:   ./build.sh --profile $PROFILE --install"
    echo "    Try it:    tools/nwdaf-local --build $BUILD_DIR   (synthetic Open5GS data)"
fi
[[ $PROFILE == minimal ]] && warn "the minimal profile is dev-legacy: HTTP/1.1 only, not Rel-18 transport compliant"
exit 0
