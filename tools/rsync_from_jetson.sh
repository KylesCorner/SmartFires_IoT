#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 DESTINATION [SOURCE]" >&2
  echo "Example: $0 /media/external/SmartFires jetson-field:/mnt/nvme_drive/data/" >&2
  exit 2
fi

destination="$1"
source_path="${2:-jetson-field:/mnt/nvme_drive/data/}"

mkdir -p "$destination"
rsync -avh --progress "$source_path" "$destination/"
