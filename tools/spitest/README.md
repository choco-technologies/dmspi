# spitest - Manual dmspi Device Test Tool

A small DMOD application module that exercises a real SPI transfer against
an already-configured `dmspi` device and reports PASS/FAIL. It does not
configure, create, or know anything about pins/instances/roles up front -
it only takes a device path (e.g. `/dev/dmspi1`, produced by `dmdevfs` from
a config.ini - see [`../../configs/README.md`](../../configs/README.md))
and reads that device's role (master/slave) back from it via `ioctl`.

It sends two distinct, known byte patterns - one MOSI-bound
(master->slave), one MISO-bound (slave->master) - and checks each side
received exactly what the other side sent, in both directions.

## Building

Built together with dmspi itself (added via `add_subdirectory()` from the
top-level `CMakeLists.txt`):

```bash
cmake -B build
cmake --build build
```

The resulting module file is produced at `build/dmf/spitest.dmf`, alongside
`build/dmf/dmspi.dmf`.

## Usage

From `dmell`, once the device(s) you want to test have already been
configured by `dmdevfs` (see [`../../configs/README.md`](../../configs/README.md)
for ready-made per-board configs, or write your own):

```
spitest <device_path>
spitest <device_path> <peer_device_path>
```

### Single device (two-board test)

Run this on each board separately - one board's device must be configured
`role=master`, the other's `role=slave`, wired together per
[`../../configs/README.md`](../../configs/README.md)'s wiring notes:

```
spitest /dev/dmspi1
```

Prints which role the device reports, runs the appropriate side of the
transfer, and reports PASS/FAIL.

### Two devices (single-board loopback)

Give it both device paths and it opens both, confirms one is master and the
other is slave (in either order), and runs the transfer between them - the
slave side in a worker thread so both sides are active at the same time:

```
spitest /dev/dmspi1 /dev/dmspi2
```

## Exit codes

- `0` - PASS
- `1` - communication/data mismatch, or `-h`/`--help` with no other args
- non-zero (from `main`, no specific code) - setup failure: device path
  couldn't be opened, or it doesn't answer `dmspi_ioctl_cmd_get_role`
  (i.e. it isn't a dmspi device)

On a data mismatch, each mismatching byte is logged individually
(`byte N: expected 0x.., got 0x..`) to help pinpoint a wiring or timing
issue - e.g. a MOSI/MISO swap will show every byte mismatching against the
*other* direction's pattern.
