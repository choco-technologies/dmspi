# DMSPI Configuration Guide

## Configuration File Format

DMSPI uses INI-format configuration files parsed by the DMINI module.

## Configuration Parameters

All parameters are in the `[dmspi]` section (or a board/device-named section,
e.g. `[flash_spi]` - the section is auto-detected the same way DMGPIO/DMUART
do it):

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `instance` | integer | 1 | SPI instance number (1-6, depending on family/board) |
| `role` | string | "master" | `"master"` or `"slave"` |
| `baudrate` | integer | (required for master) | Requested SCK frequency in Hz. Ignored for `role=slave` |
| `mode` | integer | - | SPI mode shorthand 0-3 (sets both `clock_polarity`/`clock_phase`); overridden by explicit keys below when present |
| `clock_polarity` | string | "low" | `"low"` or `"high"` (CPOL) |
| `clock_phase` | string | "1edge" | `"1edge"` or `"2edge"` (CPHA) |
| `bit_order` | string | "msb_first" | `"msb_first"` or `"lsb_first"` |
| `nss_mode` | string | "soft" | `"soft"` (peripheral ignores NSS; an optional GPIO friend can provide CS) or `"hard"` (peripheral drives/reads the physical NSS pin) |
| `cs_active_level` | string | "low" | `"low"` or `"high"` - which level the `chip_select` GPIO friend is driven to when selecting the device |
| `interrupt_trigger` | string | "off" | `"off"`, `"rx_not_empty"`, `"tx_empty"`, or `"error"` |
| `interrupt_handler` | string | (none) | Name of a dmhaman-registered handler to call on interrupt |
| `rx_ring_size` | integer | 0 | Capacity of the RX ring buffer, in bytes (0 disables it; useful mainly for `role=slave`, where reception isn't driven by the local CPU) |
| `rx_ring_wait_mode` | string | "some_data" | RX ring read-wait behavior: `"none"`, `"some_data"`, `"all_data"` |

Data frames are always 8 bits; there is no `data_size` parameter.

## Chip select

The SPI peripheral itself only ever drives clock/data lines and, in
`nss_mode=hard`, a single NSS pin. Real deployments almost always need
more flexibility than that: one bus master talking to several slaves, each
with its own CS line, or a CS pin the SPI peripheral doesn't own directly.
For the common case of **one** fixed slave, configure CS as a separate
`dmgpio` section. Give the GPIO and SPI sections the same `friends_group`,
and mark the GPIO with `friend_role=chip_select`. `dmdevfs` reports that
GPIO's device path to dmspi through `dmdrvi_friend_changed()`. dmspi then
opens the path and calls `dmgpio_ioctl_cmd_set_pins_state` around every
`write`/`read`/`dmspi_ioctl_cmd_transfer` call.

The signal pins are declared first, the SPI controller second, and CS last.
They use `driver_order=2`, after external RAM initialization at order 1.
dmspi does not currently use DMA, so it does not need a later ordering group
than DMA. `dmdevfs` uses stable insertion within the group, ensuring dmspi
exists before a CS friend can be announced, including when configuration is
added to an already mounted `dmdevfs` instance:

```ini
[flash_spi_sck]
driver_name=dmgpio
driver_order=2
friends_group=flash_spi
pin=PA5
mode=alternate
alternate_function=5

[flash_spi]
driver_name=dmspi
driver_order=2
friends_group=flash_spi
instance=1
role=master
baudrate=4000000
mode=0
nss_mode=soft
cs_active_level=low

[flash_spi_cs]
driver_name=dmgpio
driver_order=2
friends_group=flash_spi
friend_role=chip_select
pin=PA4
mode=output
pull=up
speed=maximum
output_circuit=push_pull
```

If you have more than one slave on the bus, or your CS line needs
different timing/handling than "assert before, deassert after every call",
do not add a `chip_select` friend and manage CS yourself around whatever
calls you make into dmspi - see
[`tools/spitest/main.c`](../tools/spitest/main.c) for the shape of that
(it doesn't use a CS friend since it talks to a peer board/loopback pair, not
a fixed slave device).

## Examples

### Master, mode 0, 1 MHz, CS managed via a GPIO friend

```ini
[spi]
driver_name=dmspi
driver_order=2
friends_group=example_spi
instance=1
role=master
baudrate=1000000
mode=0
nss_mode=soft

[spi_cs]
driver_name=dmgpio
driver_order=2
friends_group=example_spi
friend_role=chip_select
pin=PA4
mode=output
pull=up
```

### Master talking to a device that needs mode 3

```ini
[dmspi]
instance=2
role=master
baudrate=4000000
mode=3
bit_order=msb_first
```

### Slave with an interrupt-driven RX ring

```ini
[dmspi]
instance=3
role=slave
nss_mode=hard
interrupt_trigger=rx_not_empty
rx_ring_size=256
rx_ring_wait_mode=some_data
```

## Pre-configured Boards

See [`../configs/README.md`](../configs/README.md) for ready-made,
`dmdevfs`-loadable per-board (SPI pins + the section above bundled together)
and per-MCU configurations.

## Usage via dmdevfs (normal case)

Once a config like the ones above has been applied, `dmdevfs` creates a
device node for it under `/dev` (path/name depend on the config's
`driver_name`/section - see `configs/README.md`). From there, use it like
any other file, through the portable `Dmod_File*`/`Dmod_Ioctl` SAL wrappers
(declared in `dmod.h` itself - no extra module to link, they forward to
`dmvfs` under the hood):

```c
#include "dmod.h"
#include "dmspi_types.h"

void* fp = Dmod_FileOpen("/dev/dmspi1", "r+");

// True full-duplex transfer (e.g. reading a register while sending its address)
uint8_t tx[4] = { 0x03, 0x00, 0x00, 0x00 };
uint8_t rx[4];
dmspi_transfer_t xfer = { .tx = tx, .rx = rx, .size = sizeof(tx) };
Dmod_Ioctl(fp, dmspi_ioctl_cmd_transfer, &xfer);

// Read back which role this device was configured with
dmspi_role_t role;
Dmod_Ioctl(fp, dmspi_ioctl_cmd_get_role, &role);

Dmod_FileClose(fp);
```

See [`../tools/spitest/main.c`](../tools/spitest/main.c) for a complete,
build-tested tool built this way (it takes a device path, reads its role
back, and runs the appropriate side of a loopback test - no config or pins
guessed or hardcoded).

`spitest` itself doesn't configure a `chip_select` friend (it talks to a peer
board or a loopback pair, not a fixed slave device), so it doesn't demonstrate
CS management - see "Chip select" above for that.

## Usage without dmdevfs (advanced)

Talking to dmspi directly - without a `dmdevfs`-mounted path - is possible
but lower-level: dmspi is a separate loadable module, so its `dmdrvi` entry
points (`dmdrvi_dmspi_create`, `_open`, `_ioctl`, ...) live in a different
binary than the caller's and cannot be called by name. Resolve them
dynamically instead, the same way `dmdevfs` itself does (see
`prepare_driver_module()`/`configure_driver()` in `dmdevfs.c`): load/enable
the module by name, then look up each entry point with
`Dmod_GetDifFunction()`.

```c
#include "dmspi.h"
#include "dmdrvi.h"
#include "dmini.h"

#define ENABLE_DIF_REGISTRATIONS ON   /* before including dmdrvi.h */
#include "dmdrvi.h"

Dmod_Context_t *dmspi_module = Dmod_LoadModuleByName("dmspi");
Dmod_EnableModule("dmspi", true, NULL);

dmod_dmdrvi_create_t dmdrvi_create = (dmod_dmdrvi_create_t)
    Dmod_GetDifFunction(dmspi_module, dmod_dmdrvi_create_sig);
dmod_dmdrvi_open_t   dmdrvi_open   = (dmod_dmdrvi_open_t)
    Dmod_GetDifFunction(dmspi_module, dmod_dmdrvi_open_sig);
dmod_dmdrvi_ioctl_t  dmdrvi_ioctl  = (dmod_dmdrvi_ioctl_t)
    Dmod_GetDifFunction(dmspi_module, dmod_dmdrvi_ioctl_sig);
/* ...dmod_dmdrvi_close_sig / dmod_dmdrvi_free_sig the same way */

dmini_context_t config = dmini_create();
dmini_set_int(config, "dmspi", "instance", 1);
dmini_set_string(config, "dmspi", "role", "master");
dmini_set_int(config, "dmspi", "baudrate", 1000000);

dmdrvi_dev_num_t dev_num = {0};
dmdrvi_context_t spi_ctx = dmdrvi_create(config, &dev_num);
void* handle = dmdrvi_open(spi_ctx, DMDRVI_O_RDWR, &dev_num);
```

Prefer the `dmdevfs`/`dmvfs` path above unless you have a specific reason
not to go through it (e.g. writing `dmdevfs` itself).
