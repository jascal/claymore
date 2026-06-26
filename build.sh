#!/usr/bin/env bash
# Build claymore — one small binary (cpp-httplib + nlohmann/json, both vendored). TLS is used for HTTPS llm-synthesis
# endpoints (e.g. api.openai.com); if OpenSSL isn't available it falls back to a plain build (http synthesis only).
set -e
mkdir -p build
COMMON="-O2 -std=c++17 -Wno-deprecated-declarations -Ithird_party claymore.cpp -o build/claymore -lpthread"
if g++ -DCPPHTTPLIB_OPENSSL_SUPPORT $COMMON -lssl -lcrypto 2>/dev/null; then
  echo "built: build/claymore (with TLS — llm-synthesis over https supported)"
else
  g++ $COMMON
  echo "built: build/claymore (no TLS — llm-synthesis must use an http endpoint)"
fi
