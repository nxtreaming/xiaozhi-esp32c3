#!/usr/bin/env python3
"""
Script to flash GIF files to ESP32 storage partition

This script helps you upload GIF files to the ESP32's Flash storage partition.
It uses the SPIFFS image generation tool and esptool to flash the files.

Usage:
    python scripts/flash_gif_files.py --port COM3 --gif-dir ./gifs
    python scripts/flash_gif_files.py --port /dev/ttyUSB0 --gif-dir ./gifs --partition-size 3M
"""

import os
import sys
import argparse
import subprocess
import tempfile
import shutil
from pathlib import Path

# Default partition configuration (should match partitions_16M_small_ota.csv)
DEFAULT_PARTITION_OFFSET = 0xA00000  # 10MB offset
DEFAULT_PARTITION_SIZE = 0x600000    # 6MB size
DEFAULT_PARTITION_NAME = "storage"

def find_idf_path():
    """Find ESP-IDF installation path"""
    idf_path = os.environ.get('IDF_PATH')
    if not idf_path:
        print("Error: IDF_PATH environment variable not set")
        print("Please run: export IDF_PATH=/path/to/esp-idf")
        sys.exit(1)
    return idf_path

def find_spiffsgen_tool(idf_path):
    """Find spiffsgen.py tool in ESP-IDF"""
    spiffsgen = os.path.join(idf_path, 'components', 'spiffs', 'spiffsgen.py')
    if not os.path.exists(spiffsgen):
        print(f"Error: spiffsgen.py not found at {spiffsgen}")
        sys.exit(1)
    return spiffsgen

def find_esptool():
    """Find esptool.py"""
    # Try to find esptool in PATH
    result = shutil.which('esptool.py')
    if result:
        return result
    
    # Try to find in ESP-IDF
    idf_path = find_idf_path()
    esptool = os.path.join(idf_path, 'components', 'esptool_py', 'esptool', 'esptool.py')
    if os.path.exists(esptool):
        return esptool
    
    print("Error: esptool.py not found")
    print("Please install esptool: pip install esptool")
    sys.exit(1)

def parse_size(size_str):
    """Parse size string like '3M' or '1024K' to bytes"""
    size_str = size_str.upper().strip()
    if size_str.endswith('M'):
        return int(size_str[:-1]) * 1024 * 1024
    elif size_str.endswith('K'):
        return int(size_str[:-1]) * 1024
    else:
        return int(size_str)

def validate_gif_files(gif_dir):
    """Validate GIF files in directory"""
    gif_files = list(Path(gif_dir).glob('*.gif'))
    if not gif_files:
        print(f"Error: No GIF files found in {gif_dir}")
        sys.exit(1)
    
    total_size = sum(f.stat().st_size for f in gif_files)
    print(f"Found {len(gif_files)} GIF files, total size: {total_size / 1024:.2f} KB")
    
    for gif_file in gif_files:
        print(f"  - {gif_file.name}: {gif_file.stat().st_size / 1024:.2f} KB")
    
    return gif_files, total_size

def create_spiffs_image(gif_dir, output_image, partition_size):
    """Create SPIFFS image from GIF directory"""
    idf_path = find_idf_path()
    spiffsgen = find_spiffsgen_tool(idf_path)
    
    print(f"\nCreating SPIFFS image...")
    print(f"  Source directory: {gif_dir}")
    print(f"  Output image: {output_image}")
    print(f"  Partition size: {partition_size / 1024 / 1024:.2f} MB")
    
    cmd = [
        sys.executable,
        spiffsgen,
        str(partition_size),
        gif_dir,
        output_image,
        '--page-size', '256',
        '--block-size', '4096',
        '--obj-name-len', '32'
    ]
    
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print("SPIFFS image created successfully")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error creating SPIFFS image: {e}")
        print(f"stdout: {e.stdout}")
        print(f"stderr: {e.stderr}")
        return False

def flash_spiffs_image(port, image_file, offset, baud=460800):
    """Flash SPIFFS image to ESP32"""
    esptool = find_esptool()
    
    print(f"\nFlashing SPIFFS image to ESP32...")
    print(f"  Port: {port}")
    print(f"  Offset: 0x{offset:X}")
    print(f"  Baud rate: {baud}")
    
    cmd = [
        sys.executable,
        esptool,
        '--chip', 'auto',
        '--port', port,
        '--baud', str(baud),
        'write_flash',
        f'0x{offset:X}',
        image_file
    ]
    
    try:
        result = subprocess.run(cmd, check=True)
        print("\nFlashing completed successfully!")
        return True
    except subprocess.CalledProcessError as e:
        print(f"\nError flashing image: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(
        description='Flash GIF files to ESP32 storage partition',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Flash GIFs from ./gifs directory to COM3
  python scripts/flash_gif_files.py --port COM3 --gif-dir ./gifs
  
  # Flash with custom partition size
  python scripts/flash_gif_files.py --port /dev/ttyUSB0 --gif-dir ./gifs --partition-size 3M
  
  # Flash with custom offset (if you modified partition table)
  python scripts/flash_gif_files.py --port COM3 --gif-dir ./gifs --offset 0xD00000
        """
    )
    
    parser.add_argument('--port', '-p', required=True,
                        help='Serial port (e.g., COM3 or /dev/ttyUSB0)')
    parser.add_argument('--gif-dir', '-d', required=True,
                        help='Directory containing GIF files')
    parser.add_argument('--partition-size', '-s', default='3M',
                        help='Partition size (default: 3M)')
    parser.add_argument('--offset', '-o', default=None,
                        help='Flash offset in hex (default: 0xD00000)')
    parser.add_argument('--baud', '-b', type=int, default=460800,
                        help='Baud rate (default: 460800)')
    parser.add_argument('--keep-image', action='store_true',
                        help='Keep generated SPIFFS image file')
    
    args = parser.parse_args()
    
    # Validate inputs
    if not os.path.isdir(args.gif_dir):
        print(f"Error: Directory not found: {args.gif_dir}")
        sys.exit(1)
    
    # Parse partition size
    partition_size = parse_size(args.partition_size)
    
    # Parse offset
    if args.offset:
        offset = int(args.offset, 16) if args.offset.startswith('0x') else int(args.offset)
    else:
        offset = DEFAULT_PARTITION_OFFSET
    
    # Validate GIF files
    gif_files, total_size = validate_gif_files(args.gif_dir)
    
    if total_size > partition_size:
        print(f"\nWarning: Total GIF size ({total_size / 1024 / 1024:.2f} MB) exceeds partition size ({partition_size / 1024 / 1024:.2f} MB)")
        response = input("Continue anyway? (y/n): ")
        if response.lower() != 'y':
            sys.exit(1)
    
    # Create temporary file for SPIFFS image
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=not args.keep_image) as tmp_file:
        image_file = tmp_file.name
        
        # Create SPIFFS image
        if not create_spiffs_image(args.gif_dir, image_file, partition_size):
            sys.exit(1)
        
        # Flash to ESP32
        if not flash_spiffs_image(args.port, image_file, offset, args.baud):
            sys.exit(1)
        
        if args.keep_image:
            print(f"\nSPIFFS image saved to: {image_file}")
    
    print("\n" + "="*60)
    print("SUCCESS! GIF files have been flashed to ESP32")
    print("="*60)
    print("\nYou can now use these GIFs in your code:")
    for gif_file in gif_files:
        print(f'  app.ShowGifFromFlash("{gif_file.name}", 0, 0);')
    print()

if __name__ == '__main__':
    main()

