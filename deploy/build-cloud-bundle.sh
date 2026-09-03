#!/bin/bash
# build-cloud-bundle.sh — Build pulseTrader for Ubuntu 24.04 (cloud VPS target)
#
# Why: a locally-compiled binary (glibc 2.43 / gcc 15 / spdlog 1.15 / fmt 10)
# cannot run on Ubuntu 24.04 (glibc 2.39 / gcc 13 / spdlog 1.12 / fmt 9).
# This script compiles inside an ubuntu:24.04 container (fast, local CPU),
# runs the full test suite, then extracts a self-contained bundle:
#   deploy/cloud-out/pulseTrader/{pulsetrader, lib/, .env, *.toml, run.sh}
# which can be scp'd to the VPS and run with zero apt/sudo/docker on the target.
#
# Usage: ./deploy/build-cloud-bundle.sh
# Requires: docker (local), source .env with GATE_TESTNET_* exports.

set -euo pipefail
cd "$(dirname "$0")/.."          # repo root

IMAGE=pulse-ub24:latest
CTX=deploy/cloud-context
OUT=deploy/cloud-out/pulseTrader

echo "==> [1/5] staging header-only deps the engine was validated against"
# toml11 (host ~/.local install) + websocketpp (host apt git-2025 snapshot —
# noble's 0.8.2 predates C++20 out-of-class ctor rules and does not compile;
# /usr/local/include takes precedence over the apt copy in /usr/include)
rm -rf "$CTX/usr" "$OUT"
mkdir -p "$CTX/usr/local/include" "$CTX/usr/local/share/cmake"
cp -r ~/.local/include/toml.hpp "$CTX/usr/local/include/"
cp -r ~/.local/include/toml11   "$CTX/usr/local/include/"
cp -r ~/.local/share/cmake/toml11 "$CTX/usr/local/share/cmake/"
cp -r /usr/include/websocketpp "$CTX/usr/local/include/"

echo "==> [2/5] docker build (ubuntu:24.04 + gcc-13 + apt deps + Release compile)"
cat > "$CTX/Dockerfile" <<'EOF'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake make pkg-config git ca-certificates \
      nlohmann-json3-dev libspdlog-dev libfmt-dev libasio-dev \
      libcurl4-openssl-dev libssl-dev libwebsocketpp-dev \
      libsqlite3-dev libsqlitecpp-dev libgtest-dev tzdata \
      libcurl4t64 libspdlog1.12 libfmt9 \
    && rm -rf /var/lib/apt/lists/*
# toml11 is not packaged in 24.04 — staged from the host's ~/.local install
# (paths resolve against the build context = repo root)
COPY deploy/cloud-context/usr/ /usr/
WORKDIR /src
COPY . /src/
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DPULSE_ENABLE_SQLITE=ON \
      -DCMAKE_CXX_FLAGS="-Wno-error=array-bounds -Wno-error=stringop-overread -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized" \
 && cmake --build build -j"$(nproc)"
EOF

# .dockerignore (repo-root context): never let .env / build dirs into the image
cat > .dockerignore <<'EOF'
.git
build
build_headless
build_no_sqlite
logs
data
.env
*.db
deploy/cloud-out
EOF

# container DNS gets Clash fake-IPs (198.18.0.x) → apt unreachable;
# proxy via host Clash through the docker bridge gateway (host loopback is invisible in-container)
GW=$(docker network inspect bridge --format '{{(index .IPAM.Config 0).Gateway}}')
docker build -t "$IMAGE" -f "$CTX/Dockerfile" \
  --build-arg HTTP_PROXY="http://$GW:7897" \
  --build-arg HTTPS_PROXY="http://$GW:7897" \
  --build-arg http_proxy="http://$GW:7897" \
  --build-arg https_proxy="http://$GW:7897" \
  .

echo "==> [3/5] running full test suite inside the container (deployment TZ)"
docker run --rm -e TZ=Asia/Shanghai "$IMAGE" bash -lc 'cd /src && ctest --test-dir build --output-on-failure -j1' | tail -25

echo "==> [4/5] extracting bundle (binary + the two .so missing on the VPS)"
CID=$(docker create "$IMAGE")
trap 'docker rm -f "$CID" >/dev/null 2>&1 || true' EXIT
mkdir -p "$OUT/lib"
docker cp "$CID:/src/build/apps/pulsetrader/pulsetrader" "$OUT/pulsetrader"
# copy the real files and name them by SONAME (loader opens libspdlog.so.1.12 / libfmt.so.9)
docker cp "$CID:/usr/lib/x86_64-linux-gnu/libspdlog.so.1.12.0" "$OUT/lib/libspdlog.so.1.12"
docker cp "$CID:/usr/lib/x86_64-linux-gnu/libfmt.so.9.1.0"      "$OUT/lib/libfmt.so.9"
docker rm -f "$CID" >/dev/null 2>&1 || true
trap - EXIT

echo "==> [5/5] assembling runnable bundle"
# testnet-only env: virtual funds; mainnet keys never leave this machine
grep -E '^export GATE_TESTNET_' .env        > "$OUT/.env"
grep -E '^export PULSE_NETWORK=' .env        >> "$OUT/.env" || echo "export PULSE_NETWORK=testnet" >> "$OUT/.env"
# cloud config: no local Clash proxy; silence private-WS noise isn't needed on smoke
sed 's#proxyUrl.*#proxyUrl            = ""#' trading.testnet.toml > "$OUT/trading.testnet.toml"
cat > "$OUT/run.sh" <<'EOF'
#!/bin/bash
# run.sh — pulseTrader cloud bundle launcher (testnet keys in .env)
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TZ=Asia/Shanghai
[ -f .env ] && source .env
exec ./pulsetrader "$@"
EOF
chmod +x "$OUT/run.sh" "$OUT/pulsetrader"

echo ""
echo "============================================="
echo "Bundle ready: $OUT"
du -sh "$OUT"
ls -l "$OUT" "$OUT/lib"
echo "VPS-side install is pure copy: scp -r $OUT api@VPS:~/pulseTrader"
