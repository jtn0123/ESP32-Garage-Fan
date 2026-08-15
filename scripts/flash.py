#!/usr/bin/env python3
"""
Consolidated ESP32 firmware flashing script.
Replaces multiple redundant scripts with a single, feature-rich tool.
"""

import argparse
import glob
import os
import subprocess
import sys
import time


def run(cmd: list[str], check: bool = True) -> int:
    """Run a command and optionally check for errors."""
    print("$", " ".join(cmd))
    result = subprocess.call(cmd)
    if check and result != 0:
        print(f"ERROR: Command failed with exit code {result}")
        sys.exit(result)
    return result


def run_output(cmd: list[str]) -> str:
    """Run a command and capture output."""
    try:
        return subprocess.check_output(cmd, text=True).strip()
    except subprocess.CalledProcessError:
        return ""


def find_usb_ports() -> list[str]:
    """Find USB serial ports, excluding Bluetooth ports."""
    # Common patterns for USB serial ports on different platforms
    patterns = [
        "/dev/tty.usbmodem*",  # macOS USB
        "/dev/ttyUSB*",  # Linux USB
        "/dev/ttyACM*",  # Linux USB ACM
        "/dev/cu.usbserial*",  # macOS USB serial
        "/dev/cu.usbmodem*",  # macOS USB modem
    ]

    # Patterns to exclude (Bluetooth and other non-USB)
    exclude_patterns = [
        "*Bluetooth*",
        "*bluetooth*",
        "*BT*",
        "*Wireless*",
        "*AirPod*",
        "*debug*",
    ]

    ports: list[str] = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))

    # Filter out excluded patterns
    filtered_ports: list[str] = []
    for port in ports:
        skip = False
        for exclude in exclude_patterns:
            if exclude.replace("*", "") in port:
                skip = True
                break
        if not skip:
            filtered_ports.append(port)

    # Remove macOS duplicates (prefer cu.* over tty.*)
    # On macOS, /dev/cu.* and /dev/tty.* are the same device
    unique_ports: dict[str, str] = {}
    for port in filtered_ports:
        if "/dev/cu." in port:
            # Extract device name after cu.
            device_name = port.replace("/dev/cu.", "")
            unique_ports[device_name] = port
        elif "/dev/tty." in port:
            # Only add tty if cu version doesn't exist
            device_name = port.replace("/dev/tty.", "")
            if device_name not in unique_ports:
                unique_ports[device_name] = port
        else:
            # Non-macOS ports, add directly
            unique_ports[port] = port

    return sorted(unique_ports.values())  # Return sorted list of unique ports


def wait_for_device(timeout: int = 30) -> str | None:
    """Wait for a USB device to appear."""
    print(f"Waiting for USB device (timeout: {timeout}s)...")
    print("Instructions:")
    print("  1. Connect ESP32 via USB")
    print("  2. If device is stuck, hold BOOT button while connecting")
    print("")

    start_time = time.time()
    spinner = ["|", "/", "-", "\\"]
    spin_idx = 0

    while time.time() - start_time < timeout:
        ports = find_usb_ports()
        if ports:
            print(f"\n✓ Found device on {ports[0]}")
            return ports[0]

        # Show spinner
        print(f"\r{spinner[spin_idx]} Waiting... ", end="", flush=True)
        spin_idx = (spin_idx + 1) % len(spinner)
        time.sleep(0.5)

    print("\n✗ Timeout - no device found")
    return None


def erase_flash(port: str) -> bool:
    """Erase flash memory (recovery mode)."""
    print("\n=== ERASING FLASH ===")
    print("This will completely erase the device...")

    # Try multiple esptool locations
    esptool_commands = [
        ["python3", "-m", "esptool"],
        ["esptool.py"],
        ["~/.platformio/penv/bin/python", "-m", "esptool"],
    ]

    for esptool in esptool_commands:
        cmd = esptool + ["--chip", "esp32s2", "--port", port, "erase_flash"]

        result = run(cmd, check=False)
        if result == 0:
            print("✓ Flash erased successfully")
            return True

    print("✗ Failed to erase flash")
    return False


def bump_version() -> tuple[str | None, str | None]:
    """Create an empty commit to generate a new version hash."""
    print("\n=== BUMPING VERSION ===")

    # Get current version
    old_version = run_output(["git", "rev-parse", "--short", "HEAD"])

    # Create empty commit
    result = run(["git", "commit", "--allow-empty", "-m", "Build version bump"], check=False)
    if result != 0:
        print("Warning: Could not create version bump commit")
        return None, None

    # Get new version
    new_version = run_output(["git", "rev-parse", "--short", "HEAD"])

    if old_version != new_version:
        print(f"✓ Version bumped: {old_version} → {new_version}")
        return old_version, new_version
    else:
        print("Version unchanged")
        return old_version, old_version


def list_ports_command() -> list[str]:
    """List available USB ports for user reference."""
    ports = find_usb_ports()
    if ports:
        print("Found USB serial ports:")
        for i, port in enumerate(ports, 1):
            print(f"  {i}. {port}")
        return ports
    else:
        print("No USB serial ports found.")
        print("\nTroubleshooting:")
        print("1. Make sure your ESP32 is connected via USB")
        print("2. Check if the device appears with: ls /dev/tty.*")
        print("3. On macOS, look for /dev/tty.usbmodem* or /dev/cu.usbmodem*")
        print("4. On Linux, look for /dev/ttyUSB* or /dev/ttyACM*")
        return []


def monitor_device(port: str) -> None:
    """Monitor serial output from device."""
    print(f"\n=== MONITORING {port} ===")
    print("Press Ctrl+C to exit")
    print("")

    cmd = ["pio", "device", "monitor", "-p", port, "-b", "115200"]
    try:
        run(cmd, check=False)
    except KeyboardInterrupt:
        print("\nMonitoring stopped")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Consolidated ESP32 firmware flashing tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --env dev --mode always --monitor    # Development mode with monitoring
  %(prog)s --recover --wait                     # Recovery mode
  %(prog)s --bump-version                       # Flash with new version
  %(prog)s --list                               # List available ports
""",
    )

    # Port selection
    parser.add_argument("--port", help="Serial port (auto-detect if not specified)")
    parser.add_argument(
        "--list", action="store_true", help="List available USB serial ports and exit"
    )
    parser.add_argument("--wait", action="store_true", help="Wait for device to appear")

    # Build configuration
    parser.add_argument(
        "--env",
        choices=["display", "dev", "headless"],
        default="display",
        help="Build environment: display (production), dev (debug), headless (no display)",
    )
    parser.add_argument(
        "--mode",
        choices=["3m", "5m", "30m", "1h", "2h", "always"],
        default="1h",
        help="Sleep mode: 3m, 5m, 30m, 1h (default), 2h, or always (no sleep)",
    )

    # Actions
    parser.add_argument("--build-only", action="store_true", help="Only build, do not upload")
    parser.add_argument("--recover", action="store_true", help="Recovery mode: erase and reflash")
    parser.add_argument(
        "--monitor", action="store_true", help="Monitor serial output after flashing"
    )
    parser.add_argument(
        "--bump-version", action="store_true", help="Create commit to bump version number"
    )

    args = parser.parse_args()

    # Handle --list flag
    if args.list:
        list_ports_command()
        return 0

    # Bump version if requested
    if args.bump_version:
        old_ver, new_ver = bump_version()

    proj = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
    arduino_dir = os.path.join(proj, "firmware", "arduino")

    # Choose PlatformIO environment based on --env
    if args.env == "dev":
        env = "dev_display"  # New development environment with debugging
    elif args.env == "headless":
        env = "feather_esp32s2_headless"
    else:
        env = "feather_esp32s2_display_only"

    # Build EXTRA_FLAGS to control sleep behavior
    extra_flags = []
    if args.mode == "always":
        extra_flags.append("-DDEV_NO_SLEEP=1")
    else:
        # Use WAKE_INTERVAL to override generated_config via environment
        if args.mode == "3m":
            os.environ["WAKE_INTERVAL"] = "3m"
        elif args.mode == "5m":
            os.environ["WAKE_INTERVAL"] = "5m"
        elif args.mode == "30m":
            os.environ["WAKE_INTERVAL"] = "30m"
        elif args.mode == "1h":
            os.environ["WAKE_INTERVAL"] = "1h"
        elif args.mode == "2h":
            os.environ["WAKE_INTERVAL"] = "2h"

    # Pass dynamic flags into PlatformIO via EXTRA_FLAGS
    if extra_flags:
        os.environ["EXTRA_FLAGS"] = " ".join(extra_flags)

    # Find or wait for port (skip if build-only)
    port = args.port
    if not args.build_only and not port:
        if args.wait:
            port = wait_for_device()
            if not port:
                print("ERROR: No device found after timeout")
                return 1
        else:
            ports = find_usb_ports()
            if not ports:
                print("\nERROR: No USB serial ports found!")
                print("Use --wait to wait for device, or connect and try again")
                return 1
            elif len(ports) == 1:
                port = ports[0]
                print(f"Auto-detected USB port: {port}")
            else:
                print("\nMultiple USB ports found:")
                for i, p in enumerate(ports, 1):
                    print(f"  {i}. {p}")
                print("\nPlease specify which port to use with --port")
                return 1

    # Recovery mode: erase first
    if args.recover:
        if not erase_flash(port):
            print("ERROR: Failed to erase flash")
            print("Try putting device in bootloader mode:")
            print("  1. Hold BOOT button")
            print("  2. Connect USB while holding BOOT")
            print("  3. Release BOOT after 3-4 seconds")
            return 1
        print("\nFlash erased. Proceeding with upload...")
        time.sleep(2)

    # Build command
    base = ["pio", "run", "-d", arduino_dir, "-e", env]

    if args.build_only:
        result = run(base)
        if result == 0:
            print("\n✓ Build successful")
            if args.bump_version:
                print(f"Version: {new_ver}")
        return result

    # Upload command
    upload = base + ["-t", "upload", "--upload-port", port]

    print(f"\n=== FLASHING {env} to {port} ===")
    if args.bump_version and "new_ver" in locals():
        print(f"Version: {old_ver} → {new_ver}")
    print(f"Mode: {args.mode}")
    print("")

    result = run(upload, check=False)

    if result == 0:
        print("\n✓ Upload successful!")

        # Monitor if requested
        if args.monitor:
            time.sleep(2)  # Give device time to reset
            monitor_device(port)
    else:
        print("\n✗ Upload failed")
        print("\nTroubleshooting:")
        print("1. Try --recover flag for recovery mode")
        print("2. Hold BOOT button while connecting USB")
        print("3. Check USB cable and connections")

    return result


if __name__ == "__main__":
    sys.exit(main())
