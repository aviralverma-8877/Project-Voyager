#!/usr/bin/env python3
"""
Project Voyager - Web Flash Build Script

Builds the *release* firmware (DEBUG=false) and the LittleFS image for every
hardware version, copies the binaries into webflash/, and regenerates
webflash/manifest-<n>.json with offsets read from the built partitions.bin.

Usage:
    python3 scripts/build_webflash.py [--yes] [--clean]

PlatformIO is located via the PIO environment variable, then `pio` on PATH,
then the default PlatformIO Core install (~/.platformio/penv/bin/pio).

Author: Aviral Verma
Repository: https://github.com/aviralverma-8877/Project-Voyager
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# Hardware version -> release environment (see platformio.ini)
ENVIRONMENTS = {
    1: 'esp_lora_v1_release',
    2: 'esp_lora_v2_release',
}

REPO_ROOT = Path(__file__).resolve().parent.parent
WEBFLASH_DIR = REPO_ROOT / 'webflash'
BUILD_DIR = REPO_ROOT / '.pio' / 'build'

# Fixed by the ESP32 ROM / Arduino-ESP32 defaults.
BOOTLOADER_OFFSET = 0x1000
PARTITION_TABLE_OFFSET = 0x8000

# Partition table binary format (ESP-IDF gen_esp32part.py)
PART_ENTRY_SIZE = 32
PART_MAGIC = 0x50AA          # bytes AA 50, little-endian
PART_MD5_MAGIC = 0xEBEB
TYPE_APP, TYPE_DATA = 0x00, 0x01
SUBTYPE_APP_FACTORY, SUBTYPE_APP_OTA0 = 0x00, 0x10
SUBTYPE_DATA_OTA = 0x00
SUBTYPE_DATA_FS = (0x82, 0x83)   # spiffs, littlefs (default.csv labels LittleFS "spiffs")

DEFAULT_MANIFEST = {
    'name': 'Project Voyager v{n} - LoRa File Transfer',
    'version': '1.0.0',
    'new_install_prompt_erase': True,
}


class Colors:
    HEADER = '\033[95m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'


def print_header(message):
    line = '=' * 60
    print(f"\n{Colors.HEADER}{Colors.BOLD}{line}\n{message:^60}\n{line}{Colors.ENDC}\n")


def print_step(message):
    print(f"{Colors.OKCYAN}> {message}{Colors.ENDC}")


def print_success(message):
    print(f"{Colors.OKGREEN}OK {message}{Colors.ENDC}")


def print_error(message):
    print(f"{Colors.FAIL}ERROR {message}{Colors.ENDC}")


def print_warning(message):
    print(f"{Colors.WARNING}WARN {message}{Colors.ENDC}")


class BuildError(Exception):
    pass


# --------------------------------------------------------------------------- pio

def find_pio():
    """Return the pio executable: $PIO, then PATH, then ~/.platformio/penv."""
    env_pio = os.environ.get('PIO')
    if env_pio:
        candidate = Path(env_pio).expanduser()
        if candidate.is_file():
            return str(candidate)
        found = shutil.which(env_pio)
        if found:
            return found
        raise BuildError(f"PIO={env_pio} does not point to an executable")

    found = shutil.which('pio') or shutil.which('platformio')
    if found:
        return found

    penv = Path.home() / '.platformio' / 'penv'
    for candidate in (penv / 'bin' / 'pio', penv / 'Scripts' / 'pio.exe'):
        if candidate.is_file():
            return str(candidate)

    raise BuildError("PlatformIO not found. Install it (https://platformio.org/install) "
                     "or set PIO=/path/to/pio")


def run_command(args, description):
    """Run a command from the repo root; on failure print stdout+stderr and raise."""
    print_step(f"{description}...")
    result = subprocess.run(args, cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print_error(f"{description} failed (exit {result.returncode})")
        print(f"$ {' '.join(args)}")
        if result.stdout:
            print("----- stdout -----")
            print(result.stdout)
        if result.stderr:
            print("----- stderr -----")
            print(result.stderr)
        raise BuildError(f"{description} failed")
    print_success(f"{description} completed")
    return result


def core_dir():
    return Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))


def find_boot_app0():
    path = (core_dir() / 'packages' / 'framework-arduinoespressif32' /
            'tools' / 'partitions' / 'boot_app0.bin')
    if not path.is_file():
        raise BuildError(f"boot_app0.bin not found at {path}")
    return path


# ------------------------------------------------------------- partition table

def parse_partition_table(data):
    """Parse an ESP32 partitions.bin into a list of dicts."""
    entries = []
    for pos in range(0, len(data) - PART_ENTRY_SIZE + 1, PART_ENTRY_SIZE):
        entry = data[pos:pos + PART_ENTRY_SIZE]
        magic, = struct.unpack_from('<H', entry, 0)
        if magic in (PART_MD5_MAGIC, 0xFFFF):
            break
        if magic != PART_MAGIC:
            raise BuildError(f"bad partition entry magic 0x{magic:04X} at byte {pos}")
        ptype, subtype, offset, size = struct.unpack_from('<BBII', entry, 2)
        label = entry[12:28].split(b'\0', 1)[0].decode('ascii', 'replace')
        entries.append({'type': ptype, 'subtype': subtype, 'offset': offset,
                        'size': size, 'label': label})
    if not entries:
        raise BuildError("partition table is empty")
    return entries


def flash_layout(partitions_bin):
    """Return {'otadata','app','fs'} partitions (dicts) from a partitions.bin."""
    parts = parse_partition_table(Path(partitions_bin).read_bytes())

    def first(pred, what):
        for p in parts:
            if pred(p):
                return p
        raise BuildError(f"{what} partition not found in {partitions_bin}")

    otadata = first(lambda p: p['type'] == TYPE_DATA and p['subtype'] == SUBTYPE_DATA_OTA,
                    'otadata')
    # With otadata present, boot_app0 selects ota_0; fall back to factory.
    app = next((p for p in parts if p['type'] == TYPE_APP and p['subtype'] == SUBTYPE_APP_OTA0),
               None) or first(lambda p: p['type'] == TYPE_APP and
                              p['subtype'] == SUBTYPE_APP_FACTORY, 'ota_0/factory app')
    fs = first(lambda p: p['type'] == TYPE_DATA and p['subtype'] in SUBTYPE_DATA_FS,
               'spiffs/littlefs data')
    return {'otadata': otadata, 'app': app, 'fs': fs}


def build_manifest(version, layout, existing=None):
    """Build the ESP Web Tools manifest for one hardware version."""
    base = dict(DEFAULT_MANIFEST)
    base['name'] = base['name'].format(n=version)
    if existing:
        for key in ('name', 'version', 'new_install_prompt_erase',
                    'new_install_improv_wait_time', 'funding_url'):
            if key in existing:
                base[key] = existing[key]
    base['builds'] = [{
        'chipFamily': 'ESP32',
        'parts': [
            {'path': 'bootloader.bin', 'offset': BOOTLOADER_OFFSET},
            {'path': 'partitions.bin', 'offset': PARTITION_TABLE_OFFSET},
            {'path': 'boot_app0.bin', 'offset': layout['otadata']['offset']},
            {'path': f'firmware-{version}.bin', 'offset': layout['app']['offset']},
            {'path': f'littlefs-{version}.bin', 'offset': layout['fs']['offset']},
        ],
    }]
    return base


# ------------------------------------------------------------------ build steps

def clean_build_directory():
    print_step("Cleaning previous builds...")
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
        print_success("Build directory cleaned")
    else:
        print_success("No previous builds to clean")


def build_version(pio, version, env_name):
    print_header(f"Building Hardware Version {version} ({env_name})")
    run_command([pio, 'run', '-e', env_name], f"Building v{version} release firmware")
    run_command([pio, 'run', '-e', env_name, '--target', 'buildfs'],
                f"Building v{version} LittleFS image")


def require(path):
    if not path.is_file():
        raise BuildError(f"required file missing: {path}")
    return path


def copy(src, dst):
    shutil.copy2(src, dst)
    print_success(f"{src.relative_to(REPO_ROOT) if REPO_ROOT in src.parents else src}"
                  f" -> {dst.relative_to(REPO_ROOT)} ({src.stat().st_size / 1024:.1f} KiB)")


def package():
    print_header("Copying Files to WebFlash Directory")
    WEBFLASH_DIR.mkdir(exist_ok=True)

    # All versions share bootloader/partitions/boot_app0, so they must agree.
    shared = None
    layouts = {}
    for version, env_name in ENVIRONMENTS.items():
        env_dir = BUILD_DIR / env_name
        firmware = require(env_dir / 'firmware.bin')
        littlefs = require(env_dir / 'littlefs.bin')
        bootloader = require(env_dir / 'bootloader.bin')
        partitions = require(env_dir / 'partitions.bin')

        if shared is None:
            shared = (bootloader, partitions)
        elif (bootloader.read_bytes() != shared[0].read_bytes() or
              partitions.read_bytes() != shared[1].read_bytes()):
            raise BuildError(f"{env_name} bootloader/partitions differ from "
                             f"{ENVIRONMENTS[1]}; manifests share one copy")

        layout = flash_layout(partitions)
        for kind, binary in (('app', firmware), ('fs', littlefs)):
            if binary.stat().st_size > layout[kind]['size']:
                raise BuildError(f"{binary} ({binary.stat().st_size} B) exceeds partition "
                                 f"'{layout[kind]['label']}' ({layout[kind]['size']} B)")
        layouts[version] = layout

        copy(firmware, WEBFLASH_DIR / f'firmware-{version}.bin')
        copy(littlefs, WEBFLASH_DIR / f'littlefs-{version}.bin')

    copy(shared[0], WEBFLASH_DIR / 'bootloader.bin')
    copy(shared[1], WEBFLASH_DIR / 'partitions.bin')
    copy(find_boot_app0(), WEBFLASH_DIR / 'boot_app0.bin')
    return layouts


def write_manifests(layouts):
    print_header("Generating Manifests From partitions.bin")
    for version, layout in layouts.items():
        path = WEBFLASH_DIR / f'manifest-{version}.json'
        existing = json.loads(path.read_text()) if path.is_file() else None
        manifest = build_manifest(version, layout, existing)
        path.write_text(json.dumps(manifest, indent=2) + '\n')
        offsets = ', '.join(f"{p['path']}@0x{p['offset']:X}"
                            for p in manifest['builds'][0]['parts'])
        print_success(f"{path.name}: {offsets}")


def verify_files():
    print_header("Verifying WebFlash Files")
    required = ['bootloader.bin', 'partitions.bin', 'boot_app0.bin']
    for version in ENVIRONMENTS:
        required += [f'firmware-{version}.bin', f'littlefs-{version}.bin',
                     f'manifest-{version}.json']
    missing = [name for name in required if not (WEBFLASH_DIR / name).is_file()]
    for name in required:
        if name in missing:
            print_error(f"{name:25} MISSING")
        else:
            print_success(f"{name:25} {(WEBFLASH_DIR / name).stat().st_size / 1024:>10.1f} KiB")
    if missing:
        raise BuildError(f"missing from webflash/: {', '.join(missing)}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('-y', '--yes', action='store_true',
                        help="non-interactive: don't prompt (no clean unless --clean)")
    parser.add_argument('--clean', action='store_true',
                        help="remove .pio/build before building")
    args = parser.parse_args(argv)

    print_header("Project Voyager - WebFlash Build Tool")
    try:
        pio = find_pio()
        print_success(f"PlatformIO: {pio}")

        clean = args.clean
        if not clean and not args.yes:
            clean = input(f"\n{Colors.WARNING}Clean previous builds? (y/N): "
                          f"{Colors.ENDC}").strip().lower() == 'y'
        if clean:
            clean_build_directory()

        for version, env_name in ENVIRONMENTS.items():
            build_version(pio, version, env_name)

        layouts = package()
        write_manifests(layouts)
        verify_files()
    except BuildError as exc:
        print_error(str(exc))
        return 1

    print_header("Build Complete")
    print(f"Binaries and manifests are in {WEBFLASH_DIR}\n")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(f"\n{Colors.WARNING}Build interrupted by user.{Colors.ENDC}")
        sys.exit(130)
