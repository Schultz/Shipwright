#!/bin/bash
# Builds and runs the offline PoC test against the live RA set.
# Needs a built tree (cmake -DENABLE_RETROACHIEVEMENTS=ON) and a creds file:
#   line 1 username, line 2 web API key (unused), line 3 login token.
# Award, leaderboard and ping requests are blocked; nothing is unlocked.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build-cmake"
OUT="${TMPDIR:-/tmp}/ra_harness_build"
mkdir -p "$OUT"
CMD=$(cd "$B" && ninja -t commands soh/soh-macos | grep "RAMemory.cpp.o" | head -1)
FLAGS=$(echo "$CMD" | sed -E 's#^/usr/bin/c\+\+ ##; s# -MD -MT .*##; s# -o [^ ]+ -c [^ ]+$##')
for f in "$ROOT/RA/poc-test/ra_harness.cpp" \
         "$ROOT/soh/soh/Network/RetroAchievements/RAMemory.cpp" \
         "$ROOT/soh/soh/Network/RetroAchievements/RAHttp.cpp"; do
  (cd "$B" && eval /usr/bin/c++ $FLAGS -w -c "$f" -o "$OUT/$(basename "$f" .cpp).o")
done
/usr/bin/c++ -arch arm64 "$OUT"/*.o "$B/soh/librcheevos.a" "$B/_deps/spdlog-build/libspdlogd.a" -lcurl -o "$OUT/ra_harness"
"$OUT/ra_harness" "$1"
