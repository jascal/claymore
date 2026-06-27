#!/usr/bin/env bash
# Build claymore — one small binary (cpp-httplib + nlohmann/json, both vendored). TLS is used for HTTPS llm-synthesis
# endpoints (e.g. api.openai.com); if OpenSSL isn't available it falls back to a plain (http-only) build.
#   ./build.sh           release (-O2)
#   ./build.sh --debug   -g -O1 + ASan/UBSan + -Wall -Wextra (catch memory/UB bugs in the server)
set -e
cd "$(dirname "$0")"
mkdir -p build

DEBUG=0; for a in "$@"; do [ "$a" = "--debug" ] && DEBUG=1; done
if [ "$DEBUG" = 1 ]; then MODE="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined"; echo "[*] debug build: ASan + UBSan + warnings";
else MODE="-O2"; fi
# -isystem (not -I) for the vendored headers so their warnings don't drown ours; -Wall/-Wextra on claymore.cpp.
CXXFLAGS="$MODE -std=c++17 -Wall -Wextra -Wno-deprecated-declarations -isystem third_party"

# OpenSSL? probe the header (cheap) so the real build below shows OUR warnings instead of hiding stderr (the old
# `2>/dev/null` fallback swallowed warnings too).
SSL=""; SSLLIBS=""
if echo '#include <openssl/ssl.h>' | g++ -E -xc++ - >/dev/null 2>&1; then SSL="-DCPPHTTPLIB_OPENSSL_SUPPORT"; SSLLIBS="-lssl -lcrypto"; fi

# compile_commands.json for clang-tidy / LSP. Absolute paths → gitignored.
printf '[{"directory": "%s", "file": "claymore.cpp", "command": "g++ %s %s claymore.cpp"}]\n' \
    "$PWD" "$SSL" "$CXXFLAGS" > compile_commands.json

g++ $SSL $CXXFLAGS claymore.cpp -o build/claymore -lpthread $SSLLIBS
echo "built: build/claymore$([ "$DEBUG" = 1 ] && echo ' [debug: ASan+UBSan]') ($([ -n "$SSL" ] && echo 'with TLS — llm-synthesis over https supported' || echo 'no TLS — llm-synthesis must use an http endpoint'))"
