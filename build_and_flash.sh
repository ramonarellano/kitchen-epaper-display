#!/bin/zsh
# Build the firmware and copy the UF2 file to the RPI-RP2 drive

set -e

# Set Pico SDK path
export PICO_SDK_PATH="$PWD/pico-sdk"

# Clean and create build directory
rm -rf build
mkdir build
cd build

# Build the project
cmake ..
make -j4

# Find the UF2 file
UF2_FILE="epd.uf2"

# Find the RPI-RP2 mount point (macOS typical)
RPI_MOUNT=$(mount | grep -i RPI-RP2 | awk '{print $3}')

if [[ -z "$RPI_MOUNT" ]]; then
  echo "RPI-RP2 drive not found. Please connect your Pico in bootloader mode."
  exit 1
fi

# Copy the UF2 file
cp "$UF2_FILE" "$RPI_MOUNT/"
echo "UF2 file copied to $RPI_MOUNT."
