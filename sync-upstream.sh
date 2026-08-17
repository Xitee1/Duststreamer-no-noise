#!/usr/bin/env bash
# Sync the mirrored Duststreamer repo because he blocked me for no reason and I can't fork because of that.

set -euo pipefail

REPO_DIR="$HOME/projects/tools/Duststreamer.git"

cd "$REPO_DIR"

echo "==> Fetching from upstream..."
git fetch upstream --tags --prune

echo "==> Pushing branches to origin..."
git push origin --all

echo "==> Pushing tags to origin..."
git push origin --tags

echo "==> Sync complete."