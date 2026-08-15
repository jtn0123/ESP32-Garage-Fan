#!/usr/bin/env python3
"""
Consolidated serial monitoring script for ESP32.
Provides colored output, auto-detection, and logging features.
"""

import argparse
from datetime import datetime
import glob
import os
import subprocess
import sys
from typing import IO


# ANSI color codes
class Colors:
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    PURPLE = "\033[95m"
    CYAN = "\033[96m"
    WHITE = "\033[97m"
    RESET = "\033[0m"
    BOLD = "\033[1m"


def find_usb_ports() -> list[str]:
    """Find USB serial ports, excluding Bluetooth."""
    patterns = [
        "/dev/tty.usbmodem*",
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
        "/dev/cu.usbserial*",
        "/dev/cu.usbmodem*",
    ]

    exclude_patterns = ["Bluetooth", "bluetooth", "BT", "Wireless", "AirPod", "debug"]

    ports: list[str] = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))

    # Filter out excluded patterns
    filtered: list[str] = []
    for port in ports:
        if not any(exc in port for exc in exclude_patterns):
            filtered.append(port)

    return sorted(set(filtered))


def colorize_line(line: str) -> str:
    """Apply color highlighting to important log lines."""
    # Boot stages
    if "[BOOT-1]" in line:
        return f"{Colors.RED}● {line}{Colors.RESET}"
    elif "[BOOT-2" in line:
        return f"{Colors.YELLOW}● {line}{Colors.RESET}"
    elif "[BOOT-3]" in line:
        return f"{Colors.BLUE}● {line}{Colors.RESET}"
    elif "[BOOT-4]" in line:
        return f"{Colors.GREEN}● {line}{Colors.RESET}"
    elif "[BOOT-5]" in line:
        return f"{Colors.PURPLE}● {line}{Colors.RESET}"

    # NeoPixel status
    elif "[NEOPIXEL]" in line:
        if "Red" in line:
            return f"{Colors.RED}◉ {line}{Colors.RESET}"
        elif "Yellow" in line:
            return f"{Colors.YELLOW}◉ {line}{Colors.RESET}"
        elif "Blue" in line:
            return f"{Colors.BLUE}◉ {line}{Colors.RESET}"
        elif "Green" in line:
            return f"{Colors.GREEN}◉ {line}{Colors.RESET}"
        elif "Purple" in line:
            return f"{Colors.PURPLE}◉ {line}{Colors.RESET}"
        else:
            return f"{Colors.CYAN}◉ {line}{Colors.RESET}"

    # Display messages
    elif "[DISPLAY]" in line:
        return f"{Colors.CYAN}▢ {line}{Colors.RESET}"

    # Errors and warnings
    elif "ERROR" in line or "FAIL" in line:
        return f"{Colors.RED}{Colors.BOLD}✗ {line}{Colors.RESET}"
    elif "WARNING" in line or "WARN" in line:
        return f"{Colors.YELLOW}⚠ {line}{Colors.RESET}"

    # Success messages
    elif "SUCCESS" in line or "OK" in line or "✓" in line:
        return f"{Colors.GREEN}✓ {line}{Colors.RESET}"

    # Version info
    elif "FW Version:" in line or "Version:" in line:
        return f"{Colors.BOLD}{line}{Colors.RESET}"

    # ESP32 boot sequence header
    elif "=== ESP32 BOOT SEQUENCE ===" in line:
        return f"\n{Colors.BOLD}{Colors.CYAN}{line}{Colors.RESET}"
    elif "=== " in line and "===" in line:
        return f"{Colors.BOLD}{line}{Colors.RESET}"

    # WiFi/MQTT status
    elif "WiFi connected" in line or "MQTT connected" in line:
        return f"{Colors.GREEN}⟲ {line}{Colors.RESET}"
    elif "WiFi disconnected" in line or "MQTT disconnected" in line:
        return f"{Colors.RED}⊗ {line}{Colors.RESET}"

    # Default
    return line


def monitor_serial(
    port: str, baud: int = 115200, save_file: str | None = None, highlight: bool = True
) -> None:
    """Monitor serial output with optional highlighting and logging."""
    print(f"{Colors.BOLD}=== SERIAL MONITOR ==={Colors.RESET}")
    print(f"Port: {port}")
    print(f"Baud: {baud}")
    if save_file:
        print(f"Logging to: {save_file}")
    print(f"Highlighting: {'ON' if highlight else 'OFF'}")
    print("\nPress Ctrl+C to exit")
    print("-" * 50)
    print()

    log_file: IO[str] | None = None
    if save_file:
        try:
            log_file = open(save_file, "a")
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log_file.write(f"\n\n=== Monitor session started: {timestamp} ===\n")
            log_file.flush()
        except Exception as e:
            print(f"Warning: Could not open log file: {e}")
            log_file = None

    try:
        # Use PlatformIO's monitor which handles DTR/RTS correctly
        cmd = ["pio", "device", "monitor", "-p", port, "-b", str(baud), "--raw"]

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            bufsize=1,
        )

        assert proc.stdout is not None  # stdout=PIPE above guarantees it
        for line in proc.stdout:
            line = line.rstrip()

            # Apply highlighting if enabled
            display_line = colorize_line(line) if highlight else line
            print(display_line)

            # Log raw line to file if enabled
            if log_file:
                log_file.write(line + "\n")
                log_file.flush()

        proc.wait()

    except KeyboardInterrupt:
        print(f"\n{Colors.YELLOW}Monitor stopped by user{Colors.RESET}")
    except Exception as e:
        print(f"{Colors.RED}Error: {e}{Colors.RESET}")
    finally:
        if log_file:
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log_file.write(f"=== Monitor session ended: {timestamp} ===\n")
            log_file.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Serial monitor with colored output for ESP32",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Color coding:
  ● Boot stages:  Red→Yellow→Blue→Green
  ◉ NeoPixel:     Shows current color
  ▢ Display:      Cyan for display messages
  ✓ Success:      Green
  ✗ Error:        Red bold
  ⚠ Warning:      Yellow
  ⟲ Connected:    Green (WiFi/MQTT)
  ⊗ Disconnected: Red (WiFi/MQTT)

Examples:
  %(prog)s                     # Auto-detect port
  %(prog)s --port /dev/tty.usbmodem01
  %(prog)s --save debug.log    # Log to file
  %(prog)s --no-color          # Plain text output
""",
    )

    parser.add_argument("--port", help="Serial port (auto-detect if not specified)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--save", metavar="FILE", help="Save output to file")
    parser.add_argument("--no-color", action="store_true", help="Disable colored output")
    parser.add_argument("--list", action="store_true", help="List available ports and exit")

    args = parser.parse_args()

    # List ports if requested
    if args.list:
        ports = find_usb_ports()
        if ports:
            print("Available USB serial ports:")
            for i, port in enumerate(ports, 1):
                print(f"  {i}. {port}")
        else:
            print("No USB serial ports found")
        return 0

    # Find or use specified port
    port = args.port
    if not port:
        ports = find_usb_ports()
        if not ports:
            print(f"{Colors.RED}No USB serial ports found!{Colors.RESET}")
            print("\nTroubleshooting:")
            print("1. Connect your ESP32 via USB")
            print("2. Check device manager / system info")
            print("3. Try --list to see all ports")
            return 1
        elif len(ports) == 1:
            port = ports[0]
            print(f"{Colors.GREEN}Auto-detected port: {port}{Colors.RESET}")
        else:
            print("Multiple ports found:")
            for i, p in enumerate(ports, 1):
                print(f"  {i}. {p}")
            print("\nPlease specify with --port")
            return 1

    # Check if port exists
    if not os.path.exists(port):
        print(f"{Colors.RED}Port {port} does not exist{Colors.RESET}")
        return 1

    # Start monitoring
    try:
        monitor_serial(port, baud=args.baud, save_file=args.save, highlight=not args.no_color)
    except Exception as e:
        print(f"{Colors.RED}Failed to monitor: {e}{Colors.RESET}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
