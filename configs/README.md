# DMSPI Configuration Files

This directory contains pre-configured SPI settings for various development
boards and MCUs, following the same layout as `dmuart`/`dmgpio`/`dmclk`.
These are what `dmdevfs` (or `dmf-get --config-dir`/`--config-map`) actually
loads to bring the driver up dynamically - not throwaway samples.

**Every pin assignment below is sourced from that board's own ST user
manual, schematic, or a working third-party board config (Zephyr's device
tree) - never assumed by analogy with another board.** Two boards
(F746G-DISCO, F769I-DISCOVERY) turned out to route their Arduino SPI header
through **SPI2** on completely different pins than the "obvious" SPI1
PA5/PA6/PA7 that works on the Nucleo-64 boards, because their larger pin
count means PA5-PA7 are already claimed by USB HS ULPI/DCMI/Ethernet - a
concrete example of why this isn't guessable per-family. Each file's
comments cite the specific source and, for the accelerometer/gyroscope
boards, flag which detail (usually just the exact CS pin) came from a
secondary source rather than the primary schematic. Always cross-check
against your own board revision before flashing.

## Directory Structure

```
configs/
├── board/                          # Board-specific configurations
│   ├── nucleo-f401re/
│   │   └── spi1.ini
│   ├── nucleo-f411re/
│   │   └── spi1.ini
│   ├── nucleo-f446re/
│   │   └── spi1.ini
│   ├── nucleo-f767zi/
│   │   └── spi1.ini
│   ├── stm32f4-discovery/
│   │   └── spi1.ini
│   ├── stm32f429i-discovery/
│   │   └── spi5.ini
│   ├── stm32f746g-disco/
│   │   └── spi2.ini
│   └── stm32f769i-discovery/
│       └── spi2.ini
└── mcu/                             # MCU-specific (pin-agnostic) configurations
    ├── stm32f401re.ini
    ├── stm32f405rg.ini
    ├── stm32f407vg.ini
    ├── stm32f411re.ini
    ├── stm32f429zi.ini
    ├── stm32f439zi.ini
    ├── stm32f446re.ini
    ├── stm32f469ni.ini
    ├── stm32f722re.ini
    ├── stm32f746zg.ini
    ├── stm32f767zi.ini
    └── stm32f769ni.ini
```

## Configuration Format

Each board configuration file contains:
- `[section]` entries with `driver_name=dmgpio` that mux the SCK/MISO/MOSI
  pins to the SPI peripheral's alternate function.
- One `[section]` with `driver_name=dmspi` for the peripheral itself.

`dmdevfs` scans a config file (or a whole directory of them) for sections
declaring `driver_name`, dynamically resolves that module's `dmdrvi`
interface (`Dmod_LoadModuleByName()` + `Dmod_GetDifFunction()` - see
[docs/configuration.md](../docs/configuration.md)), and creates one device
per section, ordered by `driver_order` (GPIO pin sections must land before
the SPI section that depends on them, hence both use the same order here -
declaration order within a `driver_order` group already puts the pins
first).

The SPI peripheral itself never drives chip-select (`nss_mode` stays
`soft` throughout this directory) - dmspi still manages it, just as a
plain `dmgpio` pin rather than a native NSS signal. Every file below
configures the relevant CS pin as its own standalone `dmgpio` device
(`[..._cs]`, declared *before* the `dmspi` section that needs it) and
references it from the `dmspi` section's `cs_path`, so dmspi
asserts/deasserts it automatically around each transfer - see
[docs/configuration.md](../docs/configuration.md)'s "Chip select" section
for exactly how (including the `/dev/dmgpio<port_index>/<name>` path
format - confirmed on real hardware, **not** a flat `/dev/<name>`) and why
it's opened lazily rather than at boot. On the four generic Arduino-header
boards this assumes a single shield device wired to D10 (the
Arduino-standard CS pin); on the accelerometer/gyroscope boards it's the
one fixed onboard device. If your setup doesn't match either (multiple
slaves, a shield that manages its own CS, ...), drop `cs_path` from the
`dmspi` section and manage CS yourself.

### Example (nucleo-f401re/spi1.ini)

```ini
[arduino_spi_sck]
driver_name=dmgpio
driver_order=3
pin=PA5
mode=alternate
alternate_function=5
speed=maximum
output_circuit=push_pull
pull=none

[arduino_spi_miso]
driver_name=dmgpio
driver_order=3
pin=PA6
mode=alternate
alternate_function=5
pull=none

[arduino_spi_mosi]
driver_name=dmgpio
driver_order=3
pin=PA7
mode=alternate
alternate_function=5
speed=maximum
output_circuit=push_pull
pull=none

[arduino_spi_cs]
driver_name=dmgpio
driver_order=3
pin=PB6
mode=output
pull=up
speed=maximum
output_circuit=push_pull

[arduino_spi]
driver_name=dmspi
driver_order=3
instance=1
role=master
baudrate=1000000
mode=0
bit_order=msb_first
nss_mode=soft
cs_path=/dev/dmgpio1/arduino_spi_cs
cs_active_level=low
```

## Board Configurations

| Board | Folder | SPI Instance | Pins | CS (`cs_path` wired) | Source | Notes |
|-------|--------|---------------|------|------------------------|--------|-------|
| NUCLEO-F401RE | `board/nucleo-f401re/` | SPI1 | PA5/PA6/PA7 | D10=PB6 | UM1724 | Arduino Uno V3 header (D13/D12/D11) |
| NUCLEO-F411RE | `board/nucleo-f411re/` | SPI1 | PA5/PA6/PA7 | D10=PB6 | UM1724 | Arduino Uno V3 header (D13/D12/D11) |
| NUCLEO-F446RE | `board/nucleo-f446re/` | SPI1 | PA5/PA6/PA7 | D10=PB6 | UM1724 | Arduino Uno V3 header (D13/D12/D11) |
| NUCLEO-F767ZI | `board/nucleo-f767zi/` | SPI1 | PA5/PA6/PA7 | D10=PD14 **(unverified)** | Zephyr `nucleo_f767zi.dts` | Arduino header; **PA7 conflicts with onboard Ethernet** - check solder bridges |
| STM32F4-DISCOVERY | `board/stm32f4-discovery/` | SPI1 | PA5/PA6/PA7 | PE3 | ST community + stm32f4-discovery.net | Onboard MEMS motion sensor (LIS3DSH/LIS302DL) |
| STM32F429I-DISCOVERY | `board/stm32f429i-discovery/` | SPI5 | PF7/PF8/PF9 | PC1 | UM1670 + schematic pack | Onboard L3GD20 gyroscope |
| STM32F746G-DISCO | `board/stm32f746g-disco/` | **SPI2** | PI1/PB14/PB15 | D10=PI0 | UM1907 Table 3 | Arduino header - **not** SPI1 (PA5-7 used by USB HS ULPI/DCMI/ETH) |
| STM32F769I-DISCOVERY | `board/stm32f769i-discovery/` | **SPI2** | PH6/PJ4/PJ3 | D10=PF7 | UM2033 | Arduino header - **not** SPI1, same reason as F746G-DISCO |

Every row above was verified against the cited source before being written
here; see each `.ini` file's own header comment for the exact quote/detail
and any remaining caveat (the `alternate_function=5` value is inferred from
the general STM32 AF-per-peripheral convention, not independently checked
per-pin, on every board here; NUCLEO-F767ZI's D10=PD14 pairing specifically
was not independently cross-checked either, unlike its SCK/MISO/MOSI - see
each file for specifics).

## MCU Configurations

Pin-agnostic default (`[dmspi]` only, no GPIO sections - the same shape as
`dmuart`'s `mcu/` configs) for each MCU covered by `dmgpio`/`dmclk`'s own
`configs/mcu/`:

| MCU | File |
|-----|------|
| STM32F401RE | `mcu/stm32f401re.ini` |
| STM32F405RG | `mcu/stm32f405rg.ini` |
| STM32F407VG | `mcu/stm32f407vg.ini` |
| STM32F411RE | `mcu/stm32f411re.ini` |
| STM32F429ZI | `mcu/stm32f429zi.ini` |
| STM32F439ZI | `mcu/stm32f439zi.ini` |
| STM32F446RE | `mcu/stm32f446re.ini` |
| STM32F469NI | `mcu/stm32f469ni.ini` |
| STM32F722RE | `mcu/stm32f722re.ini` |
| STM32F746ZG | `mcu/stm32f746zg.ini` |
| STM32F767ZI | `mcu/stm32f767zi.ini` |
| STM32F769NI | `mcu/stm32f769ni.ini` |

## Usage

Install a config alongside the module with `dmf-get`:

```bash
dmf-get dmspi@1.0 --config board/nucleo-f401re/spi1.ini --config-dir ./config
```

or point `dmdevfs` at the whole `board/<name>/` directory (together with
that board's `dmgpio`/`dmclk` config directories) to bring up every
peripheral it declares in one pass.
