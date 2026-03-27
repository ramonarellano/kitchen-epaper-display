#!/bin/zsh
# Build the firmware and copy the UF2 file to the RPI-RP2 drive

set -e

# Configure Pico SDK source.
# Prefer a local checkout at ./pico-sdk, otherwise let CMake fetch it.
if [[ -d "$PWD/pico-sdk" ]]; then
  export PICO_SDK_PATH="$PWD/pico-sdk"
  USE_LOCAL_SDK=1
  CMAKE_SDK_ARGS=("-DPICO_SDK_PATH=$PICO_SDK_PATH")
else
  unset PICO_SDK_PATH
  USE_LOCAL_SDK=0
  CMAKE_SDK_ARGS=(
    "-DPICO_SDK_FETCH_FROM_GIT=ON"
  )
fi

# Prefer the full Arm GNU toolchain installed by Homebrew cask when available.
ARM_GNU_TOOLCHAIN_BIN="/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin"
if [[ -d "$ARM_GNU_TOOLCHAIN_BIN" ]]; then
  export PATH="$ARM_GNU_TOOLCHAIN_BIN:$PATH"
  CMAKE_SDK_ARGS+=("-DPICO_TOOLCHAIN_PATH=$ARM_GNU_TOOLCHAIN_BIN")
fi

# Create build directory if it doesn't exist
mkdir -p build
cd build

# If an old cache pins a missing local SDK path, drop it.
if [[ "$USE_LOCAL_SDK" -eq 0 ]]; then
  rm -f CMakeCache.txt
fi

# Build the project
cmake "${CMAKE_SDK_ARGS[@]}" ..
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
