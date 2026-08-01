#!/bin/sh
set -eu

kryon=${1:-../kryon}
status=0

if [ ! -d "$kryon" ]; then
    echo "Kryon checkout not found: $kryon" >&2
    exit 1
fi

matches=$(
    rg -in '(^|[^[:alnum:]_])(kite|krait)([^[:alnum:]_]|$)|/(kite|krait)' \
        "$kryon"/Makefile "$kryon"/GNUmakefile "$kryon"/makefile \
        "$kryon"/README.md "$kryon"/CHANGELOG.md "$kryon"/docs \
        "$kryon"/mk "$kryon"/cmd "$kryon"/include "$kryon"/src \
        "$kryon"/tests "$kryon"/tools 2>/dev/null || true
)

if [ -n "$matches" ]; then
    echo "Kryon must not reference standalone IDE repositories:" >&2
    echo "$matches" >&2
    status=1
fi

if [ -d "$kryon/ide" ]; then
    echo "Kry-written app sources must live in Krait, not Kryon: $kryon/ide" >&2
    status=1
fi

exit "$status"
