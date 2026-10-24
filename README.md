# FNIRSI DLA driver for bitoscope

Driver for the FNIRSI DLA USB logic analyzers, for use with the bitoscope host.

The driver is installed and updated independently of bitoscope: at start-up the application and the
driver negotiate the protocol version, so older and newer versions work together.

## Supported devices

The driver asks every connected unit for its model and lists it under that name:

- FNIRSI DLA-16 Plus: supported.
- FNIRSI DLA-32 Plus: recognised, not supported yet. It is not listed; the driver's log says why.

A unit that cannot be used now, for example because another program is using it, is listed as
"FNIRSI DLA", unavailable, with the reason. A session holds its unit from connecting to closing, so
no other program takes it between captures.

## Features

- Logic analyzer: 1, 2, 4, 8 or 16 channels, sample rates from 1 MHz to 1 GHz, streamed over USB
  up to 2 Gbit/s of sample data.
- Adjustable input threshold, -5.00 V to +5.00 V (default 1.60 V).
- Signal generator: square wave (PWM) on outputs P0-P3, 1 Hz to 20 MHz, adjustable duty cycle.

## Prerequisites

Linux and Windows; macOS is not supported yet.

- CMake 3.24 or newer
- A C++23 compiler: GCC 13 or newer on Linux, MSVC from Visual Studio 2022 (or its Build Tools, with
  the "Desktop development with C++" workload) on Windows
- Git, to fetch Cap'n Proto as a submodule
- libusb-1.0

### Linux

Package names are Debian/Ubuntu ones:

```sh
sudo apt install build-essential cmake git pkg-config libusb-1.0-0-dev
```

### Windows

libusb from vcpkg, installed into `C:\vcpkg` (the same in Command Prompt and PowerShell):

```bat
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install libusb:x64-windows
```

## How to compile

### Linux

```sh
git clone --recursive https://github.com/megabyte1024/bitoscope-fnirsi-dla.git
cd bitoscope-fnirsi-dla
cmake -S third_party -B build-deps -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/deps
cmake --build build-deps --target install
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(cmake -P cmake/BitoscopeBuildJobs.cmake)
```

### Windows

Command Prompt:

```bat
git clone --recursive https://github.com/megabyte1024/bitoscope-fnirsi-dla.git
cd bitoscope-fnirsi-dla
cmake -S third_party -B build-deps -DCMAKE_INSTALL_PREFIX=%CD%/deps
cmake --build build-deps --config Release --target install
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
```

PowerShell:

```powershell
git clone --recursive https://github.com/megabyte1024/bitoscope-fnirsi-dla.git
cd bitoscope-fnirsi-dla
cmake -S third_party -B build-deps "-DCMAKE_INSTALL_PREFIX=$PWD/deps"
cmake --build build-deps --config Release --target install
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
```

## Install

bitoscope looks for drivers in the `drivers/` folder next to `bitoscope_gui`. Copy the driver
executable there, or let CMake do it by passing the folder that holds `bitoscope_gui` as the prefix:

```sh
cmake --install build --prefix /path/to/bitoscope_gui_folder
```

On Windows add `--config Release`.

## Output

- `build/bin/fnirsi_dla`: the driver executable (`build\bin\Release\fnirsi_dla.exe` on Windows)
- `build/drivers/fnirsi_dla/`: the hardware check tools `fnirsi_dla_loopback_check`,
  `fnirsi_dla_max_throughput_soak_test`, `fnirsi_dla_raw_probe` and `fnirsi_dla_pwm_check_throughput`

## License

MIT, see `LICENSE`.
