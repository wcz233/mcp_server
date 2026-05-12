#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cd "$repo_root"

echo "[mcp_server] Syncing submodules..."
git submodule sync --recursive

echo "[mcp_server] Initializing submodules..."
git submodule update --init --recursive

echo "[mcp_server] Submodule status:"
git submodule status --recursive
