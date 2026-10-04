#!/usr/bin/env python3
#
# Build a Shelly Web UI OTA zip from the project's build output.
#
# Point it at the project root (defaults to the current directory):
#
#     python3 tools/make-webui-ota-zip.py
#     python3 tools/make-webui-ota-zip.py /path/to/shelly_gen4_matter_module
#     python3 tools/make-webui-ota-zip.py --model 1-mini-gen4
#
# It reads the project name and version from build/project_description.json,
# pulls the app from build/, adds an empty filesystem image, and writes one
# shelly-gen4-matter-module-<model>-v<version>-ota.zip per Gen4 model (or only
# the one given with --model).
#
# PER-MODEL PACKAGES: the stock updater only installs an unsigned package whose
# manifest name is the unit's own Shelly app code. Any other name is treated as
# a switch to another firmware and needs Shelly's manifest signature, e.g. an
# S1G4 package on a Mini fails with "Signature verification of manifest for
# S1G4 failed". The app is the same universal firmware in every package.
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
import argparse, datetime, hashlib, json, os, sys, tempfile, zipfile

# Model -> Shelly app code, as in the manifests of the official stock packages.
MODELS = {
    "1-gen4":      "S1G4",
    "1-mini-gen4": "Mini1G4",
    "1pm-gen4":    "S1PMG4",
    "2pm-gen4":    "S2PMG4",
}
PLATFORM     = "esp32c6"
# Kept above any stock version; the device never refuses it as a downgrade.
# The real firmware version is in the app and the zip filename, not here.
MANIFEST_VER = "99.0.0"
# Must match the stock partition layout (identical on all four models).
NVS_SIZE = 0xC000
FS_SIZE  = 0xE0000
# No flash encryption on these units. Left true because that
# is the correct value if a unit ever ships with encryption enabled.
ENCRYPT  = True


def digest(path):
    data = open(path, "rb").read()
    return len(data), hashlib.sha256(data).hexdigest()


def build_zip(project_dir, project_name, version, app_bin, fs_img, model, app_code):
    stem = f"shelly-gen4-matter-module-{model}-v{version}-ota"
    zip_out = os.path.join(project_dir, f"{stem}.zip")
    paths = {"app.bin": app_bin, "fs.img": fs_img}

    def part(member, **extra):
        size, sha = digest(paths[member])
        return {"src": member, "size": size, "cs_sha256": sha, **extra}

    now = datetime.datetime.now(datetime.timezone.utc)
    manifest = {
        "name": app_code,
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
        "compatible": f"{app_code}*",
    }

    with zipfile.ZipFile(zip_out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
        for member in ("app.bin", "fs.img"):
            z.write(paths[member], arcname=member)

    print(f"Created {os.path.basename(zip_out)} ({os.path.getsize(zip_out) / 1024 / 1024:.1f} MB)")
    print(f"  {project_name} v{version}, manifest name={app_code}, version={MANIFEST_VER}")


def main():
    parser = argparse.ArgumentParser(description="Build Shelly Web UI OTA zips")
    parser.add_argument("project_dir", nargs="?", default=".")
    parser.add_argument("--model", choices=sorted(MODELS),
                        help="build only this model (default: all four)")
    args = parser.parse_args()

    project_dir = os.path.abspath(args.project_dir)
    build = os.path.join(project_dir, "build")
    desc_path = os.path.join(build, "project_description.json")
    if not os.path.isfile(desc_path):
        sys.exit(f"no build found at {build} -- run `idf.py build` in {project_dir} first")
    desc = json.load(open(desc_path))
    project_name, version = desc["project_name"], desc["project_version"]

    app_bin = os.path.join(build, f"{project_name}.bin")
    if not os.path.isfile(app_bin):
        sys.exit(f"missing build output:\n  {app_bin}")

    models = [args.model] if args.model else list(MODELS)
    with tempfile.TemporaryDirectory() as tmp:
        fs_img = os.path.join(tmp, "fs.img")
        with open(fs_img, "wb") as f:
            f.write(b"\xff" * FS_SIZE)
        for model in models:
            build_zip(project_dir, project_name, version, app_bin, fs_img,
                      model, MODELS[model])


if __name__ == "__main__":
    main()
