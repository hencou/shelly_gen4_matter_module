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
# BOOTLOADER-LESS INSTALL: this package does NOT ship or replace the bootloader.
#  - Only manifest.json, app.bin and fs.img are shipped. The stock "Shelly OS
#    loader" at 0x0 is left in place, so the stock v2.0 updater installs our app
#    into the inactive slot and points its own SH0S boot-select at it -- exactly
#    the A/B flow it uses for a normal stock update, which reliably boots the new
#    app. Shipping our own bootloader instead breaks this: the stock updater is
#    SH0S-driven, and an ESP-IDF bootloader cannot follow the SH0S boot-select,
#    so the device rolls back to stock (confirmed in the field).
#  - No otadata part: the stock loader keeps its SH0S boot state; writing an
#    ESP-IDF otadata here would corrupt it.
#  - No pt (partition table) part: the stock v2.0 updater rejects our table
#    ("Unable to parse pt"), and our layout already matches the stock one.
#  - app/fs use ptn app_0/fs_0 (as the stock package itself does); the stock
#    updater does its own A/B selection and writes the inactive slot regardless.
#
# The firmware reaches the ESP-IDF-bootloader end state on its own: on first
# boot under the stock loader it performs a one-time self-migration (writes our
# ESP-IDF bootloader to 0x0 + valid otadata, then reboots). See loader_migrate.c.
#
import datetime, hashlib, json, os, sys, tempfile, zipfile

# Fixed for the Shelly 1 Gen4 hardware.
APP_CODE     = "S1G4"
COMPATIBLE   = "S1G4*"
PLATFORM     = "esp32c6"
# Kept above any stock version; the device never refuses it as a downgrade.
# The real firmware version is in the app and the zip filename, not here.
MANIFEST_VER = "99.0.0"
# Must match the stock Shelly 1 Gen4 partition layout.
NVS_SIZE = 0xC000
FS_SIZE  = 0xE0000
# No flash encryption on these units. Left true because that
# is the correct value if a unit ever ships with encryption enabled.
ENCRYPT  = True


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
        paths = {**src, "fs.img": fs_img}

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
                # No boot/otadata/pt parts: keep the stock Shelly OS loader and
                # its SH0S boot state so the stock updater's A/B flow boots our
                # app. app/fs use app_0/fs_0 as the stock package does; the
                # updater writes the inactive slot itself.
                "nvs":     {"type": "nvs", "size": NVS_SIZE, "fill": 255, "ptn": "nvs"},
                "app":     part("app.bin", type="app", ptn="app_0", encrypt=ENCRYPT),
                "fs":      part("fs.img", type="fs", ptn="fs_0", fs_size=FS_SIZE, encrypt=ENCRYPT),
            },
            "compatible": COMPATIBLE,
        }
        manifest_path = os.path.join(tmp, "manifest.json")
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)

        order = ["manifest.json", "app.bin", "fs.img"]
        src_for = {**paths, "manifest.json": manifest_path}
        with zipfile.ZipFile(zip_out, "w", zipfile.ZIP_STORED) as z:
            for member in order:
                z.write(src_for[member], arcname=member)

    print(f"Created {os.path.basename(zip_out)} ({os.path.getsize(zip_out) / 1024 / 1024:.1f} MB)")
    print(f"  {project_name} v{version}, manifest name={APP_CODE}, version={MANIFEST_VER}")


if __name__ == "__main__":
    main()
