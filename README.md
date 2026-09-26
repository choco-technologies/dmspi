# dmspi

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmspi/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmspi/actions/workflows/ci.yml)

SPI driver module for the DMOD ecosystem, supporting both master and slave
roles.

## Description

dmspi is a hardware driver (like `dmuart`/`dmgpio`): it registers with
`dmdrvi` and is used through the generic device API (create/open/read/
write/ioctl/close), configured from an INI file via `dmini`. It supports:

- Master and slave roles
- Configurable clock polarity/phase (CPOL/CPHA, or a `mode=0..3` shorthand)
- MSB-first or LSB-first bit order
- Software or hardware NSS (chip-select) management
- Polling or interrupt-driven operation, with an optional RX ring buffer
  (useful for slave mode, where reception isn't driven by the local CPU)
- True full-duplex transfers via `dmspi_ioctl_cmd_transfer`

Like `dmuart`/`dmgpio`, dmspi only configures the SPI peripheral itself.
Pin muxing is done externally via `dmgpio`; in `nss_mode=soft`, an optional
GPIO in the same `friends_group` with `friend_role=chip_select` is discovered
through `dmdevfs` and toggled with the public GPIO ioctl through its device
path around each transaction.

See [docs/configuration.md](docs/configuration.md) for the full
configuration reference and a usage example, and
[configs/](configs/README.md) for ready-made per-board/per-MCU INI files
(the same `dmdevfs`-loadable format `dmuart`/`dmgpio`/`dmclk` use).

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmspi-local.dmd -y
dmod_loader build/dmf/test_dmspi.dmf
```

This exercises hardware-independent logic only (safe to run on the host).
For a real master<->slave SPI link on actual hardware, see
[tools/spitest/](tools/spitest/) - a small CLI tool, flashed onto a board
and run from the `dmell` shell, that opens an already-`dmdevfs`-configured
device by path (see [configs/](configs/README.md)) and exercises it. It
supports both a single-board loopback (two devices wired together) and a
two-board test (one board master, one board slave).

## Usage

The normal way to use a dmspi device is through `dmdevfs`, like any other
file: open the path `dmdevfs` created for it
(`Dmod_FileOpen("/dev/dmspi1", "r+")`), then `Dmod_Ioctl`/`Dmod_FileClose`
(portable SAL wrappers declared in `dmod.h`, no extra module to link) - see
[tools/spitest/main.c](tools/spitest/main.c) for a complete, build-tested
example.

Writing your own driver-adjacent code that talks to dmspi *without* going
through a mounted path is also possible, but lower-level: dmspi is a
separate loadable module, so its `dmdrvi` entry points
(`dmdrvi_dmspi_create`, `_open`, `_ioctl`, ...) must be resolved dynamically
via `Dmod_GetDifFunction()` rather than called by name - see
[docs/configuration.md](docs/configuration.md) for why and how. See
[docs/api-reference.md](docs/api-reference.md) for the complete API
reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation
- **[configuration.md](docs/configuration.md)** - INI configuration reference and usage example
- **[port-implementation.md](docs/port-implementation.md)** - How the port is structured and how to add another architecture

View documentation using `dmf-man dmspi`.

## Configuration Files

[configs/](configs/README.md) has ready-made, `dmdevfs`-loadable INI files
per board (SPI pins muxed via `dmgpio` + the `dmspi` section itself) and per
MCU (pin-agnostic defaults) - the same layout and mechanism `dmuart`,
`dmgpio`, and `dmclk` use.

## Hardware Port

This module ships two DMOD modules: the architecture-independent
`dmspi` and `dmspi_port`, which contains the
architecture-specific implementation. `stm32f4` and `stm32f7` are supported
today, sharing a common SPI register implementation (both families use the
same SPI IP block). The active architecture is selected via
`DMOD_CPU_FAMILY` (default: `stm32f7`):

```bash
cmake .. -DDMOD_CPU_FAMILY=stm32f7   # or stm32f4
```

See [docs/port-implementation.md](docs/port-implementation.md) for how the
common/per-family split works and how to add another architecture.
Port-specific files:

```
├── include/dmspi_port.h
├── src/port/
│   ├── CMakeLists.txt
│   ├── stm32_common/       # shared STM32 SPI register implementation
│   │   ├── stm32_common.h
│   │   └── stm32_common.c
│   ├── stm32f4/
│   │   ├── config.cmake
│   │   └── port.c
│   └── stm32f7/
│       ├── config.cmake
│       └── port.c
└── dmspi_port.dmr
```
## Project Structure

```
dmspi/
├── configs/            # per-board/per-MCU INI files - see "Configuration Files" above
│   ├── board/
│   └── mcu/
├── docs/              # Documentation (markdown format)
├── examples/
│   └── config.ini      # minimal, board-agnostic sample
├── include/           # Public headers
│   ├── dmspi.h
│   ├── dmspi_port.h
│   └── dmspi_types.h
├── src/
│   ├── dmspi.c
│   └── port/          # see "Hardware Port" above
├── tests/
│   ├── CMakeLists.txt
│   └── dmspi_test.c
├── tools/
│   └── spitest/       # hardware self-test - see "Testing" above
│       ├── CMakeLists.txt
│       ├── main.c
│       ├── README.md
│       └── spitest.dmr
├── CMakeLists.txt
├── Makefile
├── dmspi.dmr
├── dmspi_port.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
