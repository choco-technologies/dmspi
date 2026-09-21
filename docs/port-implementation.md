# Adding a New MCU Port to dmspi

`dmspi_port` currently supports `stm32f4` and `stm32f7`. The port is split
from the core `dmspi` module so a new architecture can be added without
touching architecture-independent logic.

## Current layout

```
src/port/
├── CMakeLists.txt          # selects DMOD_CPU_FAMILY's port.c + shared sources
├── stm32_common/
│   ├── stm32_common.h      # shared SPI register layout, instance table, helpers
│   └── stm32_common.c      # every dmod_dmspi_port_api_declaration(...) lives here
├── stm32f4/
│   ├── config.cmake
│   └── port.c              # dmod_init/dmod_deinit, IRQ handlers, one register hook
└── stm32f7/
    ├── config.cmake
    └── port.c              # same shape as stm32f4/port.c
```

STM32F4 and STM32F7 implement the same SPI IP block (identical CR1/CR2/SR/DR
register layout, RCC enable bits, and NVIC IRQ numbers for SPI1-SPI6), so
`stm32_common.c` owns the entire driver: register-level init, transfers,
clock/CPOL/CPHA/bit-order/NSS/baud-rate configuration, interrupt handling,
and the RX ring integration. Each family's `port.c` is a thin wrapper:
DMOD lifecycle hooks, `DMOD_IRQ_HANDLER(...)` entries that forward into the
shared `stm32_spi_irq_handler()`, and exactly one function -
`stm32_spi_family_configure_frame_format()` - which is the one place the two
SPI IP revisions genuinely diverge (STM32F7 needs `CR2.FRXTH` set so RXNE
asserts per-byte instead of per 16-bit FIFO level; STM32F4's classic IP
needs nothing beyond clearing `CR1.DFF`). See `stm32f4/port.c` and
`stm32f7/port.c` for the two (very short) implementations.

## Steps to add another architecture

1. Create `src/port/<family>/config.cmake`, setting `DMOD_TOOLS_NAME` for the
   target architecture (see `src/port/stm32f7/config.cmake` for the pattern -
   it must match a directory under `dmod/configs/arch/...`).
2. Create `src/port/<family>/port.c` implementing `dmod_init`/`dmod_deinit`
   and any `DMOD_IRQ_HANDLER(...)` needed, following
   `src/port/stm32f7/port.c` as a template.
3. If the underlying peripheral IP is identical (or nearly identical) to an
   existing family, reuse that family's `src/port/<family>_common/` instead
   of writing a new implementation - only add the register-level quirks that
   genuinely differ, following the `stm32_spi_family_configure_frame_format()`
   pattern above. Only introduce a brand new `_common/` folder for a
   genuinely different peripheral IP (e.g. a non-STM32 vendor).
4. Update `src/port/CMakeLists.txt`'s family-detection `if()` if the new
   family needs different shared sources than the `^stm32` case already
   picks up.
5. Build by selecting the new family: `cmake .. -DDMOD_CPU_FAMILY=<family>`.
6. Do not introduce a module-specific variable (e.g. `<MODULE>_MCU_SERIES`) for
   this - `DMOD_CPU_FAMILY` is the ecosystem-wide convention, already wired
   into `dmf-get` package resolution.
