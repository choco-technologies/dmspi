# dmspi API Reference

dmspi is a DMDRVI hardware driver: applications interact with it through the
generic `dmdrvi` DIF (create/open/read/write/ioctl/close/free), not through a
bespoke per-module API. Because dmspi is a separate loadable module, these
entry points (named `dmdrvi_dmspi_*`) must be resolved dynamically via
`Dmod_GetDifFunction()` rather than called by name - see
[configuration.md](configuration.md) for why and a full usage example.

## Types

See [include/dmspi_types.h](../include/dmspi_types.h) for the complete
definitions.

| Type | Description |
|------|-------------|
| `dmspi_instance_t` | SPI instance number (1-based, e.g. 1 = SPI1) |
| `dmspi_baudrate_t` | Requested SCK frequency in Hz (master mode only) |
| `dmspi_role_t` | `dmspi_role_master` / `dmspi_role_slave` |
| `dmspi_clock_polarity_t` | `dmspi_clock_polarity_low` / `_high` (CPOL) |
| `dmspi_clock_phase_t` | `dmspi_clock_phase_1_edge` / `_2_edge` (CPHA) |
| `dmspi_bit_order_t` | `dmspi_bit_order_msb_first` / `_lsb_first` |
| `dmspi_nss_mode_t` | `dmspi_nss_mode_soft` / `_hard` - see [configuration.md](configuration.md) |
| `dmspi_int_trigger_t` | Interrupt trigger bitmask: `rx_not_empty`, `tx_empty`, `error` |
| `dmspi_transfer_t` | Full-duplex transfer descriptor (`tx`, `rx`, `size`) used with `dmspi_ioctl_cmd_transfer` |
| `dmspi_config_t` | Full driver configuration (see [include/dmspi.h](../include/dmspi.h)) |

Data frames are always 8 bits wide; 16-bit frame support is not implemented.

## Functions

`dmspi_validate_config` is a plain, statically-callable function (a normal
Built-in API, linked via `dmod_link_modules(dmspi)`). Every other row is a
DIF entry point - resolve it via `Dmod_GetDifFunction()`, it is **not**
callable by name from another module (see [configuration.md](configuration.md)).

| Function | Description |
|----------|-------------|
| `dmspi_validate_config(const dmspi_config_t*)` | Validate a config struct without touching hardware. Safe to call even when `dmspi_port` isn't loaded. |
| `dmdrvi_dmspi_create(dmini_context_t, dmdrvi_dev_num_t*)` | Create a driver instance from an INI configuration. |
| `dmdrvi_dmspi_free(dmdrvi_context_t)` | Destroy an instance created by `_create`. |
| `dmdrvi_dmspi_open` / `_close` | Open/close a handle on the device. |
| `dmdrvi_dmspi_read` | Receive bytes - drains the RX ring if configured, otherwise blocks clocking dummy bytes (master) or waiting for the external master (slave). |
| `dmdrvi_dmspi_write` | Transmit bytes (full duplex at the wire level; received bytes are discarded). |
| `dmdrvi_dmspi_ioctl` | Get/set configuration parameters, register an interrupt handler, run a full-duplex transfer, or reconfigure - see `dmspi_ioctl_cmd_t` in [include/dmspi_types.h](../include/dmspi_types.h). |
| `dmdrvi_dmspi_flush` | Block until any in-progress transfer completes. |
| `dmdrvi_dmspi_stat` | Report device metadata. |

## Port API

The architecture-specific implementation lives in the separate `dmspi_port`
module (see [include/dmspi_port.h](../include/dmspi_port.h) and
[port-implementation.md](port-implementation.md)). `dmspi.c` never touches
hardware directly - every register access goes through `dmspi_port_*`.
