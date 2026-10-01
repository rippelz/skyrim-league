#!/usr/bin/env bash
set -euo pipefail
bridge_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$bridge_root/tools/manager.py" --open "$@"
