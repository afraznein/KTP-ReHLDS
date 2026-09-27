#!/bin/bash
# Build rehlds/HLTV/Proxy's proxy.so for the KTP data server, and refuse to emit
# one that cannot load there.
#
#   bash build_proxy.sh                  # -> build/proxy.so, verified
#   bash build_proxy.sh --verify <file>  # run the checks on a binary, build nothing
#   PROXY_BUILD_IMAGE=ubuntu:26.04 PROXY_MAX_GLIBC=2.43 bash build_proxy.sh
#
# WHY THIS EXISTS AT ALL
# The top-level cmake has built the Proxy target on every build.sh run for months
# (CMakeLists.txt -> add_subdirectory(rehlds/HLTV) -> Proxy/CMakeLists.txt's
# add_library(proxy SHARED)) and the output has been discarded every time, because
# build_linux.sh only ever searches build/ for engine_i486.so and hlds_linux. That
# is how a merged HLTV fix sat undeployed for over a week in August 2026. This is
# the missing half, and it is committed -- build_linux.sh is gitignored, so nothing
# in it can be relied on from a clone.
#
# WHY A CONTAINER AND NOT THE HOST TOOLCHAIN
# proxy.so is the only artifact here that ships to the data server instead of the
# 24 game hosts, and it ships as ONE file shared by all 24 HLTV instances
# (hltv@%i's WorkingDirectory is the same directory for every port). That host is
# Ubuntu 24.04: gcc 13.3.0, glibc 2.39. A build on a newer distro can link
# GLIBC_2.43 and GLIBC_ABI_GNU_TLS, load perfectly on the machine that built it,
# and fail to load on the target -- so every proxy dies at the next 03:00/11:00 ET
# restart and nothing records until a human notices. The failure is invisible
# where you build it. Hence the pinned image, and hence the checks, which are the
# point of this script rather than a formality.
#
# WHAT IT DELIBERATELY DOES NOT DO
# It does not deliver. Delivery needs root on another host and wants a canary
# first; that lives in the coordination repo's NEIN-DEPLOY.md as dpl-ca4e. The
# exact commands are printed at the end so there is no gap between the two.
set -euo pipefail

IMAGE="${PROXY_BUILD_IMAGE:-ubuntu:24.04}"
MAX_GLIBC="${PROXY_MAX_GLIBC:-2.39}"   # the target host's glibc; raise it when the host moves
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/build/proxy.so"

# What the live artifact links, verbatim. An addition here is a new runtime
# dependency on the data server, which is a deploy decision, not a build detail.
EXPECTED_NEEDED="libsteam_api.so libstdc++.so.6 libm.so.6 libc.so.6"

# --verify takes a binary this script did not build -- another maintainer's, or
# the one already live on the data server -- and runs only the checks. Same code
# path as a build's own verification, so the two cannot drift apart.
VERIFY_ONLY=0
if [ "${1:-}" = "--verify" ]; then
    [ -n "${2:-}" ] || { echo "usage: build_proxy.sh --verify <file>" >&2; exit 2; }
    VERIFY_ONLY=1
    OUT="$2"
    [ -f "$OUT" ] || { echo "ERROR: no such file: $OUT" >&2; exit 2; }
fi

build_in_container() {
    local runtime=""
    for candidate in docker podman; do
        command -v "$candidate" >/dev/null 2>&1 && { runtime="$candidate"; break; }
    done
    if [ -z "$runtime" ]; then
        echo "ERROR: need docker or podman to build against $IMAGE." >&2
        echo "       Building on this host instead is not a fallback -- see the header." >&2
        exit 1
    fi

    mkdir -p "$ROOT/build"
    echo "[build_proxy] $runtime, image $IMAGE, target glibc <= $MAX_GLIBC"

    # Source is mounted read-only and cmake writes only to /tmp/bp inside the
    # container, so a build cannot leave anything in the working tree.
    "$runtime" run --rm -v "$ROOT:/src:ro" -v "$ROOT/build:/out" "$IMAGE" bash -c '
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null
apt-get -qq install -y g++-multilib cmake make binutils >/dev/null
gcc --version | head -1
ldd --version | head -1
cmake -S /src/rehlds/HLTV/Proxy -B /tmp/bp -DCMAKE_BUILD_TYPE=Release >/tmp/cmake.log 2>&1 \
    || { tail -30 /tmp/cmake.log; exit 1; }
cmake --build /tmp/bp -j"$(nproc)" >/tmp/make.log 2>&1 \
    || { tail -40 /tmp/make.log; exit 1; }
cp /tmp/bp/proxy.so /out/proxy.so
'
    [ -f "$OUT" ] || { echo "ERROR: no artifact at $OUT" >&2; exit 1; }
}

if [ "$VERIFY_ONLY" -eq 0 ]; then
    build_in_container
else
    echo "[build_proxy] verify only: $OUT, target glibc <= $MAX_GLIBC"
fi

# ---------------------------------------------------------------- verification
# None of this is reachable by md5: a rebuild of identical source differs by
# BuildID, so md5 answers "is this the file I tested", never "will this load on
# the target".
fail=0

# 1. No symbol version the target's glibc does not have. sort -V is load-bearing:
#    a string compare puts 2.4 after 2.39 and 2.43 before it.
too_new=$(objdump -p "$OUT" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -Vu \
          | while read -r v; do
                [ "$(printf '%s\n%s\n' "$v" "$MAX_GLIBC" | sort -V | tail -1)" = "$MAX_GLIBC" ] \
                    || echo "GLIBC_$v"
            done)
if [ -n "$too_new" ]; then
    echo "FAIL: needs a newer glibc than the target ($MAX_GLIBC): $(echo $too_new)" >&2
    fail=1
fi

# 2. No GLIBC_ABI_* tag. The live artifact carries none, and GLIBC_ABI_GNU_TLS --
#    what a glibc 2.43 build adds -- is a hard load failure on 2.39.
abi_tags=$(objdump -p "$OUT" | grep -oE 'GLIBC_ABI_[A-Z_]+' | sort -u || true)
if [ -n "$abi_tags" ]; then
    echo "FAIL: carries ABI tags the target lacks: $(echo $abi_tags)" >&2
    fail=1
fi

# 3. Runtime dependencies unchanged from the live artifact.
needed=$(objdump -p "$OUT" | awk '/NEEDED/ {print $2}' | sort | tr '\n' ' ')
expected=$(printf '%s\n' $EXPECTED_NEEDED | sort | tr '\n' ' ')
if [ "$needed" != "$expected" ]; then
    echo "FAIL: DT_NEEDED changed." >&2
    echo "      expected: $expected" >&2
    echo "      got:      $needed" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    if [ "$VERIFY_ONLY" -eq 0 ]; then
        mv "$OUT" "$OUT.rejected"
        echo "" >&2
        echo "Artifact moved to $OUT.rejected so it cannot be deployed by accident." >&2
    fi
    exit 1
fi

echo ""
echo "[build_proxy] OK  $OUT"
echo "  size $(stat -c%s "$OUT")  md5 $(md5sum "$OUT" | cut -d' ' -f1)"
echo "  NEEDED $needed"
echo "  highest glibc $(objdump -p "$OUT" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' \
                        | sort -Vu | tail -1)  (target $MAX_GLIBC)"

[ "$VERIFY_ONLY" -eq 1 ] && exit 0

echo ""
echo "Not deployed. proxy.so is ONE file shared by all 24 HLTV instances and is not on"
echo "stage-wave.py's path. Canary first -- coordination NEIN-DEPLOY.md, dpl-ca4e:"
echo "  scp build/proxy.so <data-server>:/tmp/proxy.so"
echo "  sudo cp /home/hltvserver/hlds/proxy.so /home/hltvserver/hlds/proxy.so.bak-\$(date +%Y%m%d)"
echo "  sudo install -o hltvserver -g hltvserver -m 755 /tmp/proxy.so /home/hltvserver/hlds/proxy.so"
echo "Running proxies are unaffected; the next 03:00/11:00 ET restart activates it."
