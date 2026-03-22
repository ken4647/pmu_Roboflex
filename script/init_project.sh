#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_FILE="$SCRIPT_DIR/patches/xsched.patch"

cd "$REPO_ROOT"

## clone the submodules
git submodule update --init --recursive

## apply xsched patches
if [[ -f "$PATCH_FILE" ]] && [[ -e "$REPO_ROOT/third_party/xsched/.git" ]]; then
    if git -C third_party/xsched apply --check "$PATCH_FILE" 2>/dev/null; then
        git -C third_party/xsched apply "$PATCH_FILE"
        echo "Applied xsched patch"
    else
        echo "xsched patch already applied or not applicable, skipping"
    fi
fi
