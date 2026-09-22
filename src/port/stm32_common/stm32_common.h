#ifndef STM32_COMMON_H
#define STM32_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include "dmspi_port.h"

/**
 * @brief STM32 SPI register layout.
 *
 * Identical field layout on F4 and F7 (both implement the "SPI v1" IP
 * block - no FIFO). The only per-family differences are a handful of
 * CR1/CR2 bits, handled by stm32_spi_family_configure_frame_format().
 */
typedef struct
{
    volatile uint32_t CR1;      /**< Control register 1 */
    volatile uint32_t CR2;      /**< Control register 2 */
    volatile uint32_t SR;       /**< Status register */
    volatile uint32_t DR;       /**< Data register */
    volatile uint32_t CRCPR;    /**< CRC polynomial register */
    volatile uint32_t RXCRCR;   /**< RX CRC register */
    volatile uint32_t TXCRCR;   /**< TX CRC register */
    volatile uint32_t I2SCFGR;  /**< I2S configuration register */
    volatile uint32_t I2SPR;    /**< I2S prescaler register */
} stm32_spi_t;

/* SPI_CR1 bits (identical position on F4/F7, except bit 11: DFF on F4,
 * CRCL on F7 - unused by this driver, CRC support is not implemented) */
#define STM32_SPI_CR1_CPHA        (1U << 0)
#define STM32_SPI_CR1_CPOL        (1U << 1)
#define STM32_SPI_CR1_MSTR        (1U << 2)
#define STM32_SPI_CR1_BR_Pos      3U
#define STM32_SPI_CR1_BR_Msk      (0x7U << STM32_SPI_CR1_BR_Pos)
#define STM32_SPI_CR1_SPE         (1U << 6)
#define STM32_SPI_CR1_LSBFIRST    (1U << 7)
#define STM32_SPI_CR1_SSI         (1U << 8)
#define STM32_SPI_CR1_SSM         (1U << 9)
#define STM32_SPI_CR1_RXONLY      (1U << 10)
#define STM32_SPI_CR1_DFF         (1U << 11)   /**< F4 only */

/* SPI_CR2 bits (bits 8-14 - DS/FRXTH/LDMA - are F7 only) */
#define STM32_SPI_CR2_RXDMAEN     (1U << 0)
#define STM32_SPI_CR2_TXDMAEN     (1U << 1)
#define STM32_SPI_CR2_SSOE        (1U << 2)
#define STM32_SPI_CR2_ERRIE       (1U << 5)
#define STM32_SPI_CR2_RXNEIE      (1U << 6)
#define STM32_SPI_CR2_TXEIE       (1U << 7)
#define STM32_SPI_CR2_FRXTH       (1U << 12)   /**< F7 only */
#define STM32_SPI_CR2_DS_Pos      8U           /**< F7 only */
#define STM32_SPI_CR2_DS_Msk      (0xFU << STM32_SPI_CR2_DS_Pos)
#define STM32_SPI_CR2_DS_8BIT     (0x7U << STM32_SPI_CR2_DS_Pos)

/* SPI_SR bits */
#define STM32_SPI_SR_RXNE         (1U << 0)
#define STM32_SPI_SR_TXE          (1U << 1)
#define STM32_SPI_SR_CRCERR       (1U << 4)
#define STM32_SPI_SR_MODF         (1U << 5)
#define STM32_SPI_SR_OVR          (1U << 6)
#define STM32_SPI_SR_BSY          (1U << 7)

/** RCC register structure (subset shared by F4/F7 for peripheral clock enable) */
typedef struct
{
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} stm32_rcc_t;

#define STM32_RCC_BASE          0x40023800UL
#define STM32_RCC               ((volatile stm32_rcc_t *)STM32_RCC_BASE)

/* RCC_CFGR APB prescaler fields (same layout on STM32F4/F7) */
#define STM32_RCC_CFGR_PPRE1_Pos    10U
#define STM32_RCC_CFGR_PPRE1_Msk    (0x7U << STM32_RCC_CFGR_PPRE1_Pos)
#define STM32_RCC_CFGR_PPRE2_Pos    13U
#define STM32_RCC_CFGR_PPRE2_Msk    (0x7U << STM32_RCC_CFGR_PPRE2_Pos)

#define STM32_HSI_VALUE             16000000U   /**< HSI clock value in Hz, used as a
                                                   *   defensive fallback only */

/** Highest SPI instance number supported by this port (SPI1-SPI6) */
#define STM32_SPI_MAX_INSTANCES     6U

/** Default timeout (busy-wait iterations) for blocking SPI operations */
#define STM32_SPI_TIMEOUT_VALUE     100000U

/**
 * @brief Per-instance descriptor: base address, enabling RCC register/bit,
 *        and NVIC IRQ number. Identical across F4 and F7 for every instance
 *        that exists on both families (SPI1-SPI6 share base address, RCC
 *        bus/bit and IRQ number assignment on both).
 */
typedef struct
{
    uint32_t base;          /**< Peripheral base address */
    volatile uint32_t *enr; /**< RCC enable register (APB1ENR or APB2ENR) */
    uint32_t enable_bit;    /**< Bit within *enr that enables this instance's clock */
    uint32_t ppre_shift;    /**< PPRE field position in RCC_CFGR for this instance's bus */
    uint32_t irqn;          /**< NVIC IRQ number */
} stm32_spi_instance_desc_t;

/** Instance descriptor table, indexed by (instance - 1). Defined in stm32_common.c. */
extern const stm32_spi_instance_desc_t stm32_spi_instances[STM32_SPI_MAX_INSTANCES];

/**
 * @brief Apply the family-specific frame format bits.
 *
 * This driver always uses 8-bit data frames. On F4 that only requires
 * clearing DFF (CR1 bit 11); on F7 the newer FIFO-capable IP additionally
 * needs DS[3:0]=8bit and FRXTH=1 so RXNE asserts after a single byte
 * instead of waiting for a 16-bit FIFO level. Implemented once per family
 * in that family's port.c, since it is the one place the two SPI IP
 * revisions genuinely diverge.
 *
 * @param SPI Register block of the instance being configured.
 */
void stm32_spi_family_configure_frame_format(volatile stm32_spi_t *SPI);

/**
 * @brief Common EXTI-style NVIC helpers, shared by both families' port.c
 *        (declared here so stm32_common.c and the per-family port.c files
 *        agree on the same priority policy).
 */
void stm32_spi_nvic_enable_irq(uint32_t irqn);
void stm32_spi_nvic_disable_irq(uint32_t irqn);

/**
 * @brief Reset the shared interrupt-handler/RX-ring bookkeeping.
 *
 * Called once from each family's dmod_init()/dmod_deinit(), since those
 * lifecycle hooks live in the per-family port.c, not in this shared file.
 */
void stm32_spi_common_init(void);
void stm32_spi_common_deinit(void);

/**
 * @brief Shared ISR body, called from each family's DMOD_IRQ_HANDLER.
 *
 * Drains RXNE into the configured RX ring (if any) and/or dispatches to a
 * registered interrupt handler, and reports/clears error flags.
 *
 * @param instance SPI instance the firing IRQ belongs to.
 */
void stm32_spi_irq_handler(dmspi_instance_t instance);

#endif // STM32_COMMON_H
