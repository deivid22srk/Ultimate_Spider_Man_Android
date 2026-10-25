#!/bin/sh
# Regenerates gen/ (recompiled C) from your own copy of Ultimate Spider-Man
# (PC, 2005). Usage: tools/regen_gen.sh GAME_DIR
#
# GAME_DIR is the folder that contains USM.exe and binkw32.dll (the "Game Files"
# folder of your install). Nothing is copied into the repository: gen/ and
# analysis/ are git-ignored and derived from your game, so they must never be
# redistributed.
#
# Needs Python 3.10+: pip install capstone==5.0.9 pefile==2024.8.26
# (unicorn==2.0.1.post1 is only needed by tools/difftest.py)
set -e
G=$(cd "${1:?usage: tools/regen_gen.sh GAME_DIR}" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
find_file() { for f in "$G"/*; do
    [ "$(echo "${f##*/}" | tr 'A-Z' 'a-z')" = "$1" ] && { echo "$f"; return; }; done
    echo "missing $1 in $G" >&2; exit 1; }
EXE=$(find_file usm.exe)
BINK=$(find_file binkw32.dll)
mkdir -p analysis gen
export LIFT_GAME=usm
# discover.py imports peimg.py from tools/, so run it from there.
(cd tools && python3 discover.py "$EXE" ../analysis/functions_usm.json)
(cd tools && python3 discover.py "$BINK" ../analysis/functions_bink.json 09000000)
python3 tools/lift.py "$EXE" analysis/functions_usm.json gen exe
python3 tools/lift.py "$BINK" analysis/functions_bink.json gen bink 09000000
echo "gen/ is ready: $(ls gen/*.c | wc -l) files, $(du -sh gen | cut -f1)"
