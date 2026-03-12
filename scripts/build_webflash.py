#!/usr/bin/env python3
"""
Project Voyager - Web Flash Build Script
Builds firmware and filesystem for all hardware versions and prepares webflash directory

Author: Aviral Verma
Repository: https://github.com/aviralverma-8877/Project-Voyager
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path

# Configuration
ENVIRONMENTS = {
    'v1': 'esp_lora_v1',
    'v2': 'esp_lora_v2'
}

WEBFLASH_DIR = Path('webflash')
BUILD_DIR = Path('.pio/build')

# Colors for terminal output
class Colors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

def print_header(message):
    """Print formatted header"""
    print(f"\n{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{message:^60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}\n")

def print_step(message):
    """Print step message"""
    print(f"{Colors.OKCYAN}➤ {message}{Colors.ENDC}")

def print_success(message):
    """Print success message"""
    print(f"{Colors.OKGREEN}✓ {message}{Colors.ENDC}")

def print_error(message):
    """Print error message"""
    print(f"{Colors.FAIL}✗ {message}{Colors.ENDC}")

def print_warning(message):
    """Print warning message"""
    print(f"{Colors.WARNING}⚠ {message}{Colors.ENDC}")

def run_command(command, description):
    """Run shell command and handle errors"""
    print_step(f"{description}...")
    try:
        result = subprocess.run(
            command,
            shell=True,
            check=True,
            capture_output=True,
            text=True
        )
        print_success(f"{description} completed")
        return True
    except subprocess.CalledProcessError as e:
        print_error(f"{description} failed!")
        print(f"Error: {e.stderr}")
        return False

def clean_build_directory():
    """Clean previous build artifacts"""
    print_step("Cleaning previous builds...")
    if BUILD_DIR.exists():
        try:
            shutil.rmtree(BUILD_DIR)
            print_success("Build directory cleaned")
        except Exception as e:
            print_warning(f"Could not clean build directory: {e}")
    else:
        print_success("No previous builds to clean")

def build_firmware(env_name, version):
    """Build firmware for specific environment"""
    print_header(f"Building Firmware for Hardware Version {version}")

    # Build firmware
    if not run_command(
        f"pio run -e {env_name}",
        f"Building {version} firmware"
    ):
        return False

    return True

def build_filesystem(env_name='esp_lora_v1'):
    """Build LittleFS filesystem"""
    print_header("Building LittleFS Filesystem")

    if not run_command(
        f"pio run -e {env_name} --target buildfs",
        "Building LittleFS filesystem"
    ):
        return False

    return True

def copy_files_to_webflash():
    """Copy built files to webflash directory"""
    print_header("Copying Files to WebFlash Directory")

    # Ensure webflash directory exists
    WEBFLASH_DIR.mkdir(exist_ok=True)

    # Copy firmware files for each version
    for version, env_name in ENVIRONMENTS.items():
        print_step(f"Copying files for version {version}...")

        env_build_dir = BUILD_DIR / env_name

        if not env_build_dir.exists():
            print_error(f"Build directory not found: {env_build_dir}")
            return False

        # Files to copy
        files_to_copy = {
            'firmware.bin': f'firmware-{version[-1]}.bin',
            'littlefs.bin': f'littlefs-{version[-1]}.bin',
            'bootloader.bin': 'bootloader.bin',
            'partitions.bin': 'partitions.bin'
        }

        # Copy each file
        for src_name, dst_name in files_to_copy.items():
            src_file = env_build_dir / src_name
            dst_file = WEBFLASH_DIR / dst_name

            if src_file.exists():
                try:
                    shutil.copy2(src_file, dst_file)
                    size_mb = src_file.stat().st_size / (1024 * 1024)
                    print_success(f"Copied {src_name} → {dst_name} ({size_mb:.2f} MB)")
                except Exception as e:
                    print_error(f"Failed to copy {src_name}: {e}")
                    return False
            else:
                print_warning(f"File not found: {src_file}")

        # Copy boot_app0.bin (usually in the first environment)
        if version == 'v1':
            boot_app0_src = env_build_dir / 'boot_app0.bin'
            boot_app0_dst = WEBFLASH_DIR / 'boot_app0.bin'

            # Try alternative locations
            if not boot_app0_src.exists():
                boot_app0_src = Path('.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin')

            if boot_app0_src.exists():
                try:
                    shutil.copy2(boot_app0_src, boot_app0_dst)
                    print_success(f"Copied boot_app0.bin")
                except Exception as e:
                    print_warning(f"Failed to copy boot_app0.bin: {e}")

    return True

def update_manifests():
    """Update manifest files to use littlefs instead of spiffs"""
    print_header("Updating Manifest Files")

    for i in [1, 2]:
        manifest_file = WEBFLASH_DIR / f'manifest-{i}.json'

        if manifest_file.exists():
            try:
                content = manifest_file.read_text()
                # Replace spiffs with littlefs
                updated_content = content.replace(f'"spiffs-{i}.bin"', f'"littlefs-{i}.bin"')
                manifest_file.write_text(updated_content)
                print_success(f"Updated manifest-{i}.json")
            except Exception as e:
                print_warning(f"Could not update manifest-{i}.json: {e}")

def verify_files():
    """Verify all required files exist in webflash directory"""
    print_header("Verifying WebFlash Files")

    required_files = [
        'bootloader.bin',
        'partitions.bin',
        'boot_app0.bin',
        'firmware-1.bin',
        'firmware-2.bin',
        'littlefs-1.bin',
        'littlefs-2.bin',
        'manifest-1.json',
        'manifest-2.json'
    ]

    all_present = True
    for filename in required_files:
        filepath = WEBFLASH_DIR / filename
        if filepath.exists():
            size_mb = filepath.stat().st_size / (1024 * 1024)
            print_success(f"{filename:25} - {size_mb:>8.2f} MB")
        else:
            print_error(f"{filename:25} - MISSING")
            all_present = False

    return all_present

def main():
    """Main build process"""
    print_header("Project Voyager - WebFlash Build Tool")
    print(f"{Colors.BOLD}Author:{Colors.ENDC} Aviral Verma")
    print(f"{Colors.BOLD}Repository:{Colors.ENDC} https://github.com/aviralverma-8877/Project-Voyager\n")

    # Check if platformio is installed
    print_step("Checking PlatformIO installation...")
    try:
        subprocess.run(['pio', '--version'], check=True, capture_output=True)
        print_success("PlatformIO found")
    except (subprocess.CalledProcessError, FileNotFoundError):
        print_error("PlatformIO not found! Please install PlatformIO first.")
        print("Install: https://platformio.org/install")
        sys.exit(1)

    # Optional: Clean previous builds
    response = input(f"\n{Colors.WARNING}Clean previous builds? (y/N): {Colors.ENDC}")
    if response.lower() == 'y':
        clean_build_directory()

    # Build firmware for all versions
    for version, env_name in ENVIRONMENTS.items():
        if not build_firmware(env_name, version):
            print_error(f"\nBuild failed for {version}!")
            sys.exit(1)

        if not build_filesystem(env_name):
            print_error("\nFilesystem build failed!")
            sys.exit(1)

    # Copy files to webflash directory
    if not copy_files_to_webflash():
        print_error("\nFailed to copy files to webflash directory!")
        sys.exit(1)

    # Update manifest files
    update_manifests()

    # Verify all files
    if not verify_files():
        print_warning("\nSome files are missing from webflash directory!")
        sys.exit(1)

    # Success!
    print_header("Build Complete! ✓")
    print(f"{Colors.OKGREEN}All firmware and filesystem files have been built and copied to:")
    print(f"{Colors.BOLD}{WEBFLASH_DIR.absolute()}{Colors.ENDC}\n")
    print(f"{Colors.OKCYAN}You can now use these files with the ESP Web Tools flasher.{Colors.ENDC}\n")

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n\n{Colors.WARNING}Build interrupted by user.{Colors.ENDC}")
        sys.exit(1)
    except Exception as e:
        print_error(f"\nUnexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
