#!/usr/bin/env bash
# update_version.sh - sync src/version.h from the newest CHANGELOG.md entry
# (the changelog is the master truth). Usage: ./update_version.sh

set -e

CHANGELOG_FILE="CHANGELOG.md"
VERSION_H_FILE="src/version.h"

replace_in_file() {
    local expr="$1"
    local file="$2"
    local tmp

    tmp=$(mktemp "${file}.XXXXXX")
    sed "$expr" "$file" > "$tmp"
    mv "$tmp" "$file"
}

LATEST_VERSION=$(sed -n 's/^## \[\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)\].*/\1/p' \
    "$CHANGELOG_FILE" | head -n 1)

if [ -z "$LATEST_VERSION" ]; then
    echo "Error: no '## [X.Y.Z]' entry found in $CHANGELOG_FILE"
    exit 1
fi

MAJOR=$(echo "$LATEST_VERSION" | cut -d. -f1)
MINOR=$(echo "$LATEST_VERSION" | cut -d. -f2)
PATCH=$(echo "$LATEST_VERSION" | cut -d. -f3)

CURRENT=$(sed -n 's/^#define KRAIT_VERSION_STRING "\(.*\)"/\1/p' "$VERSION_H_FILE")
if [ "$LATEST_VERSION" = "$CURRENT" ]; then
    echo "Already at $LATEST_VERSION"
    exit 0
fi

echo "Updating to: $LATEST_VERSION (was $CURRENT)"

replace_in_file "s/^#define KRAIT_VERSION_MAJOR .*/#define KRAIT_VERSION_MAJOR $MAJOR/" "$VERSION_H_FILE"
replace_in_file "s/^#define KRAIT_VERSION_MINOR .*/#define KRAIT_VERSION_MINOR $MINOR/" "$VERSION_H_FILE"
replace_in_file "s/^#define KRAIT_VERSION_PATCH .*/#define KRAIT_VERSION_PATCH $PATCH/" "$VERSION_H_FILE"
replace_in_file "s/^#define KRAIT_VERSION_STRING \".*\"/#define KRAIT_VERSION_STRING \"$LATEST_VERSION\"/" "$VERSION_H_FILE"

echo "✓ Updated $VERSION_H_FILE"
echo ""
echo "Done! Build and test the app before committing."
