#ifndef ESP32S3_SPI_H
#define ESP32S3_SPI_H

#include <stdint.h>

/*
 * ESP32-S3: two general purpose SPI controllers, GP-SPI2 and GP-SPI3 (SPI0 and SPI1 are the flash and the PSRAM). The instance number is
 * the number in the name of the controller: 2 and 3. Master only here. The signals go to the pads through the GPIO matrix
 * (dmspi_port_set_pins), there are no fixed pads. Registers: ESP32-S3 Technical Reference Manual, chapter SPI Controller.
 */
#define ESP32S3_SPI_FIRST_INSTANCE      2U
#define ESP32S3_SPI_COUNT               2U
#define ESP32S3_SPI_BASE(i)             ((i) == 0U ? 0x60024000UL : 0x60025000UL)
#define ESP32S3_SPI_BUFFER_BYTES        64U             /* W0..W15: one transaction */
#define ESP32S3_SPI_CLOCK_HZ            80000000UL      /* the clock of the master (MST_CLK_SEL: 80 MHz) */

/* Registers (offsets) */
#define SPI_CMD_REG                     0x00U
#define SPI_CTRL_REG                    0x08U
#define SPI_CLOCK_REG                   0x0CU
#define SPI_USER_REG                    0x10U
#define SPI_USER1_REG                   0x14U
#define SPI_MS_DLEN_REG                 0x1CU
#define SPI_MISC_REG                    0x20U
#define SPI_DMA_CONF_REG                0x30U
#define SPI_DMA_INT_CLR_REG             0x38U
#define SPI_DMA_INT_RAW_REG             0x3CU
#define SPI_W0_REG                      0x98U
#define SPI_SLAVE_REG                   0xE0U
#define SPI_CLK_GATE_REG                0xE8U

#define SPI_CMD_UPDATE                  (1UL << 23)
#define SPI_CMD_USR                     (1UL << 24)
#define SPI_CTRL_RD_BIT_ORDER           (1UL << 23)     /* 1: LSB first */
#define SPI_CTRL_WR_BIT_ORDER           (1UL << 25)
#define SPI_CLOCK_CLK_EQU_SYSCLK        (1UL << 31)
#define SPI_USER_DOUTDIN                (1UL << 0)      /* full duplex */
#define SPI_USER_CS_HOLD                (1UL << 6)
#define SPI_USER_CS_SETUP               (1UL << 7)
#define SPI_USER_CK_OUT_EDGE            (1UL << 9)
#define SPI_USER_USR_MOSI               (1UL << 27)
#define SPI_USER_USR_MISO               (1UL << 28)
#define SPI_MISC_CS0_DIS                (1UL << 0)
#define SPI_MISC_CK_IDLE_EDGE           (1UL << 29)
/* DMA_CONF: the resets of the data path (pulsed), the DMA enables, RX_EOF_EN = 0: the end of a transaction ends the RX descriptor */
#define SPI_DMA_CONF_DMA_RX_ENA         (1UL << 27)
#define SPI_DMA_CONF_DMA_TX_ENA         (1UL << 28)
#define SPI_DMA_CONF_RX_AFIFO_RST       (1UL << 29)
#define SPI_DMA_CONF_BUF_AFIFO_RST      (1UL << 30)
#define SPI_DMA_CONF_DMA_AFIFO_RST      (1UL << 31)
#define SPI_DMA_CONF_RX_EOF_EN          (1UL << 21)
#define SPI_DMA_INT_TRANS_DONE          (1UL << 12)
#define SPI_SLAVE_SLAVE_MODE            (1UL << 26)
#define SPI_SLAVE_SOFT_RESET            (1UL << 27)
#define SPI_CLK_GATE_CLK_EN             (1UL << 0)
#define SPI_CLK_GATE_MST_CLK_ACTIVE     (1UL << 1)
#define SPI_CLK_GATE_MST_CLK_SEL        (1UL << 2)      /* 1: the 80 MHz clock */

/* System: the clocks and the resets of the controllers, PERIP_CLK_EN0 / PERIP_RST_EN0 */
#define ESP32S3_SYSTEM_BASE             0x600C0000UL
#define ESP32S3_PERIP_CLK_EN0_REG       (ESP32S3_SYSTEM_BASE + 0x18UL)
#define ESP32S3_PERIP_RST_EN0_REG       (ESP32S3_SYSTEM_BASE + 0x20UL)
#define ESP32S3_SPI_PERIP_BIT(i)        ((i) == 0U ? (1UL << 6) : (1UL << 16))

/* GPIO matrix and IO MUX */
#define ESP32S3_GPIO_BASE               0x60004000UL
#define ESP32S3_GPIO_FUNC_IN_SEL_REG(s) (ESP32S3_GPIO_BASE + 0x154UL + 4UL * (s))
#define ESP32S3_GPIO_FUNC_OUT_SEL_REG(p) (ESP32S3_GPIO_BASE + 0x554UL + 4UL * (p))
#define ESP32S3_GPIO_IN_SEL_ROUTED      (1UL << 7)
#define ESP32S3_IO_MUX_REG(pad)         (0x60009004UL + 4UL * (pad))
#define ESP32S3_IO_MUX_IE               (1UL << 9)
#define ESP32S3_IO_MUX_MCU_SEL_S        12U
#define ESP32S3_IO_MUX_MCU_SEL_M        (7UL << ESP32S3_IO_MUX_MCU_SEL_S)
#define ESP32S3_IO_MUX_FUNC_GPIO        1UL
/* Signals of the GPIO matrix (gpio_sig_map.h): the same index for the input and the output. SPI2 is FSPI, SPI3 is SPI3. */
#define ESP32S3_SPI_SIG_CLK(i)          ((i) == 0U ? 101UL : 66UL)
#define ESP32S3_SPI_SIG_MISO(i)         ((i) == 0U ? 102UL : 67UL)      /* "Q" */
#define ESP32S3_SPI_SIG_MOSI(i)         ((i) == 0U ? 103UL : 68UL)      /* "D" */
#define ESP32S3_SPI_SIG_CS0(i)          ((i) == 0U ? 110UL : 71UL)

#endif /* ESP32S3_SPI_H */
