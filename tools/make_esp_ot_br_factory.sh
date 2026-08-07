#!/usr/bin/env bash
# Build a single merged flash image for ESPConnect / web-based flashing.
#
# Usage:
#   ./tools/make_esp_ot_br_factory.sh
#   WIN_DOWNLOADS=/mnt/c/Users/<name>/Downloads ./tools/make_esp_ot_br_factory.sh
#                                         # also copies the merged bin to your Windows Downloads
#
# Prereqs:
#   - ESP-IDF env loaded (`. ~/esp-idf/export.sh` or your esp-matter IDF environment)
#   - `idf.py build` has already run (build/ must contain bootloader, PT, app, etc.)

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
OUT="${PROJECT_DIR}/esp_ot_br_factory.bin"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "ERROR: no build/ directory found — run 'idf.py build' first" >&2
    exit 1
fi

if [[ ! -f "${BUILD_DIR}/flash_args" ]]; then
    echo "ERROR: build/flash_args is missing — run 'idf.py build' first" >&2
    exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py not on PATH — source ESP-IDF first: . ~/esp-idf/export.sh" >&2
    exit 1
fi

cd "${PROJECT_DIR}"

# idf.py merge-bin reads build/flash_args and automatically picks up the
# same files and offsets 'idf.py flash' would use (bootloader,
# partition table, otadata, app image, and any extra flash targets
# such as rcp_fw/web_storage) — no manual offsets needed.
idf.py merge-bin -o "${OUT}"

SIZE=$(stat -c %s "${OUT}")
printf "\nMerged image: %s (%s bytes)\n" "${OUT}" "${SIZE}"

if [[ -n "${WIN_DOWNLOADS:-}" ]]; then
    cp "${OUT}" "${WIN_DOWNLOADS}/"
    echo "Copied to ${WIN_DOWNLOADS}/"
fi

echo
echo "Next: open ESPConnect in Chrome, Flash Tools -> Flash Firmware,"
echo "      offset 0x0, 'Erase entire flash before writing' = on."
