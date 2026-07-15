#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 TORRENT_DIR OUTPUT_JSON [COLLECTION_KEY]" >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
args=("$1" "$2")
if [[ $# -eq 3 ]]; then
  args+=(--collection-key "$3")
fi
exec python3 "$script_dir/presumed_enrichment.py" "${args[@]}"
