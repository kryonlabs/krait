#!/bin/bash
# Launch Krait the way Kryon apps run on this machine: llvmpipe software
# GL. The radeonsi hardware path stalls in Mesa (window freezes after a
# few frames, input stops); Neon's run scripts do the same.
cd "$(dirname "$0")"
export GALLIUM_DRIVER=llvmpipe
exec build/linux-x86_64/bin/krait "$@"
