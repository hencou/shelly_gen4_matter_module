#!/usr/bin/env bash
# Build a single merged flash image for ESPConnect / web-based flashing.
#
# Usage:
#   ./tools/make-merged.sh                # produces shelly_gen4_matter_module_factory.bin in project root
#   WIN_DOWNLOADS=/mnt/c/Users/user/Downloads ./tools/make_factory_bin_file.sh
#                                         # also copies the merged bin to your Windows Downloads
#
# Prereqs:
#   - ESP-IDF env loaded (`. ~/esp/esp-idf/export.sh`)
#   - `idf.py build` has completed (build/ directory must contain bootloader, PT, otadata, app)

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
OUT="${PROJECT_DIR}/shelly_gen4_matter_module_factory.bin"

REQUIRED=(
    "${BUILD_DIR}/bootloader/bootloader.bin"
    "${BUILD_DIR}/partition_table/partition-table.bin"
    "${BUILD_DIR}/ota_data_initial.bin"
    "${BUILD_DIR}/shelly_gen4_matter_module.bin"
)
for f in "${REQUIRED[@]}"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: missing $f — run 'idf.py build' first" >&2
        exit 1
    fi
done

if ! command -v esptool.py >/dev/null 2>&1; then
    echo "ERROR: esptool.py not on PATH — source ESP-IDF first: . ~/esp/esp-idf/export.sh" >&2
    exit 1
fi

esptool.py --chip esp32c6 merge_bin \
    -o "${OUT}" \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0      "${BUILD_DIR}/bootloader/bootloader.bin" \
    0x8000   "${BUILD_DIR}/partition_table/partition-table.bin" \
    0x11000  "${BUILD_DIR}/ota_data_initial.bin" \
    0x20000  "${BUILD_DIR}/shelly_gen4_matter_module.bin"

SIZE=$(stat -c %s "${OUT}")
printf "\nMerged image: %s (%s bytes)\n" "${OUT}" "${SIZE}"

if [[ -n "${WIN_DOWNLOADS:-}" ]]; then
    cp "${OUT}" "${WIN_DOWNLOADS}/"
    echo "Copied to ${WIN_DOWNLOADS}/"
fi

echo
echo "Next: open ESPConnect in Chrome, Flash Tools -> Flash Firmware,"
echo "      offset 0x0, 'Erase entire flash before writing' = on."
