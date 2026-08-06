#!/usr/bin/env python3
#
# Build a Shelly Web UI OTA zip from the project's build output.
#
# Point it at the project root (defaults to the current directory):
#
#     python3 tools/make-webui-ota-zip.py
#     python3 tools/make-webui-ota-zip.py /path/to/shelly_gen4_matter_module
#
# It reads the project name and version from build/project_description.json,
# pulls the app from build/, adds an empty filesystem image, and writes
# shelly-gen4-matter-module-v<version>-ota.zip.
#
# ESP-IDF BOOTLOADER MODE: this package installs our own ESP-IDF bootloader.
#  - Ships boot (build/bootloader/bootloader.bin) at offset 0x0. The stock
#    updater flashes it over the stock "Shelly OS loader", so from then on the
#    device boots via the standard ESP-IDF bootloader + otadata. This removes
#    the dependency on the reverse-engineered stock "SH0S" boot-select and on
#    any future changes to the Shelly loader: our OTA simply uses
#    esp_ota_set_boot_partition(). Return-to-stock re-flashes the stock app; the
#    Shelly loader is then reinstalled by the stock firmware's own next update.
#  - Ships our app into the INACTIVE slot (app_1/fs_1). The stock v2.0 updater
#    runs from app_0 and cannot rewrite the slot it is executing from, so a part
#    with ptn "app_0" is silently skipped (confirmed in the field: after such a
#    package only our bootloader landed, both app slots still held the stock
#    app). Targeting app_1 is the slot the updater can actually write.
#  - Ships an otadata that selects app_1. A fresh all-0xFF otadata makes the
#    ESP-IDF bootloader fall back to app_0 (the first bootable slot) -- which
#    here still holds the stock app -- so it must explicitly point at app_1
#    where our app was flashed. See otadata_select_slot().
#  - No pt (partition table) part is shipped. The stock v2.0 partition table
#    carries an MD5 entry, a scratch partition and per-partition encrypt flags;
#    our build's table has none of those, so the stock v2.0 updater rejects it
#    ("Unable to parse pt"). Our layout already matches the stock table (app_0
#    @0x20000, nvs @0x14000, otadata @0x11000, ...), and our bootloader reads
#    the table from 0x10000, so the device's own table is kept as-is.
#  - These units are NOT flash-encrypted (confirmed: the on-flash bootloader is
#    byte-identical to the plaintext stock bootloader.bin), so writing our
#    plaintext bootloader is safe.
#
import binascii, datetime, hashlib, json, os, struct, sys, tempfile, zipfile

# Fixed for the Shelly 1 Gen4 hardware.
APP_CODE     = "S1G4"
COMPATIBLE   = "S1G4*"
PLATFORM     = "esp32c6"
# Kept above any stock version; the device never refuses it as a downgrade.
# The real firmware version is in the app and the zip filename, not here.
MANIFEST_VER = "99.0.0"
# Must match the stock Shelly 1 Gen4 partition layout.
NVS_SIZE     = 0xC000
FS_SIZE      = 0xE0000
OTADATA_SIZE = 0x2000
SECTOR_SIZE  = 0x1000
# The stock updater runs from app_0, so it can only write the other slot.
TARGET_SLOT  = 1  # app_1 / fs_1
# No flash encryption on these units. Left true because that
# is the correct value if a unit ever ships with encryption enabled.
ENCRYPT  = True


def otadata_select_slot(slot, app_count=2):
    """ESP-IDF otadata image (two sectors) that boots the given OTA app slot.

    The bootloader picks the highest valid ota_seq and maps it to a slot via
    (ota_seq - 1) % app_count, so seq = slot + 1 selects `slot`. The entry lives
    in sector 0; sector 1 is left erased (all-0xFF = invalid). ota_state stays
    0xFFFFFFFF (UNDEFINED = boots without a pending-verify gate), matching what
    ESP-IDF's otatool writes. crc is crc32 over the ota_seq field only.
    """
    seq = slot + 1
    img = bytearray(b"\xff" * OTADATA_SIZE)
    crc = binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    # esp_ota_select_entry_t: ota_seq[4] seq_label[20] ota_state[4] crc[4]
    entry = struct.pack("<I", seq) + b"\xff" * 20 + struct.pack("<I", 0xFFFFFFFF) \
            + struct.pack("<I", crc)
    img[0:len(entry)] = entry
    return bytes(img)


def digest(path):
    data = open(path, "rb").read()
    return len(data), hashlib.sha256(data).hexdigest()


def main():
    project_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    build = os.path.join(project_dir, "build")
    desc_path = os.path.join(build, "project_description.json")
    if not os.path.isfile(desc_path):
        sys.exit(f"no build found at {build} -- run `idf.py build` in {project_dir} first")
    desc = json.load(open(desc_path))
    project_name, version = desc["project_name"], desc["project_version"]

    src = {
        "bootloader.bin":      os.path.join(build, "bootloader", "bootloader.bin"),
        "app.bin":             os.path.join(build, f"{project_name}.bin"),
    }
    missing = [p for p in src.values() if not os.path.isfile(p)]
    if missing:
        sys.exit("missing build outputs:\n  " + "\n  ".join(missing))

    stem = f"shelly-gen4-matter-module-v{version}-ota"
    zip_out = os.path.join(project_dir, f"{stem}.zip")

    with tempfile.TemporaryDirectory() as tmp:
        fs_img = os.path.join(tmp, "fs.img")
        with open(fs_img, "wb") as f:
            f.write(b"\xff" * FS_SIZE)
        boot_state = os.path.join(tmp, "boot_state.bin")
        with open(boot_state, "wb") as f:
            f.write(otadata_select_slot(TARGET_SLOT))
        paths = {**src, "fs.img": fs_img, "boot_state.bin": boot_state}

        def part(member, **extra):
            size, sha = digest(paths[member])
            return {"src": member, "size": size, "cs_sha256": sha, **extra}

        now = datetime.datetime.now(datetime.timezone.utc)
        manifest = {
            "name": APP_CODE,
            "platform": PLATFORM,
            "version": MANIFEST_VER,
            "build_id": now.strftime("%Y%m%d-%H%M%S") + f"/{stem}",
            "build_timestamp": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "parts": {
                # min_version 0.0.0: the stock updater always writes our boot,
                # replacing the stock loader at 0x0 (confirmed in the field).
                "boot":    part("bootloader.bin", type="boot", addr=0x0,
                                min_version="0.0.0", encrypt=ENCRYPT),
                "otadata": part("boot_state.bin", type="otadata", ptn="otadata",
                                encrypt=ENCRYPT),
                "nvs":     {"type": "nvs", "size": NVS_SIZE, "fill": 255, "ptn": "nvs"},
                "app":     part("app.bin", type="app", ptn=f"app_{TARGET_SLOT}", encrypt=ENCRYPT),
                "fs":      part("fs.img", type="fs", ptn=f"fs_{TARGET_SLOT}", fs_size=FS_SIZE, encrypt=ENCRYPT),
            },
            "compatible": COMPATIBLE,
        }
        manifest_path = os.path.join(tmp, "manifest.json")
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)

        order = ["manifest.json", "bootloader.bin", "boot_state.bin", "app.bin", "fs.img"]
        src_for = {**paths, "manifest.json": manifest_path}
        with zipfile.ZipFile(zip_out, "w", zipfile.ZIP_STORED) as z:
            for member in order:
                z.write(src_for[member], arcname=member)

    print(f"Created {os.path.basename(zip_out)} ({os.path.getsize(zip_out) / 1024 / 1024:.1f} MB)")
    print(f"  {project_name} v{version}, manifest name={APP_CODE}, version={MANIFEST_VER}")


if __name__ == "__main__":
    main()
