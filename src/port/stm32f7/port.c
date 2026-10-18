#define DMOD_ENABLE_REGISTRATION    ON
#include "dmspi_port.h"
#include "dmod.h"
#include "../stm32_common/stm32_common.h"

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("dmspi port module initialized (stm32f7)\n");
    stm32_spi_common_init();
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmspi port module deinitialized (stm32f7)\n");
    stm32_spi_common_deinit();
    return 0;
}

/* ---- Family-specific register quirk ----
 *
 * STM32F7 implements the newer FIFO-capable SPI IP: data size is selected
 * via CR2.DS[3:0] instead of CR1.DFF, and RXNE only asserts once the RX
 * FIFO reaches the threshold configured by CR2.FRXTH (default: 16-bit /
 * half-full). This driver only ever moves 8-bit frames, so FRXTH must be
 * set to get a per-byte RXNE like STM32F4's classic SPI IP already gives
 * for free. */
void stm32_spi_family_configure_frame_format(volatile stm32_spi_t *SPI)
{
    SPI->CR2 = (SPI->CR2 & ~STM32_SPI_CR2_DS_Msk) | STM32_SPI_CR2_DS_8BIT | STM32_SPI_CR2_FRXTH;
}

/* The DTCM is the first 64-128 KiB of RAM (by the part) - the lowest
 * 128 KiB are refused (an F74x/F75x's SRAM1 starts above 64 KiB); the ITCM
 * is below 0x00080000. DMA1 neither reaches the external memory (FMC banks,
 * 0x60000000-0xDFFFFFFF): on an STM32F746G-DISCO a TX stream reading the
 * SDRAM fails. SRAM1/2 and the AXI SRAM are reached by both. */
bool stm32_spi_family_dma_reachable(uint8_t controller, const void *buffer, size_t size)
{
    uintptr_t start = (uintptr_t)buffer;
    if (start < 0x00080000UL || (start < 0x20020000UL && start + size > 0x20000000UL))
        return false;
    return controller != 0U || !(start < 0xE0000000UL && start + size > 0x60000000UL);
}

/* ---- IRQ handlers ---- */

DMOD_IRQ_HANDLER(35) { stm32_spi_irq_handler(1); }  /* SPI1 */
DMOD_IRQ_HANDLER(36) { stm32_spi_irq_handler(2); }  /* SPI2 */
DMOD_IRQ_HANDLER(51) { stm32_spi_irq_handler(3); }  /* SPI3 */
DMOD_IRQ_HANDLER(84) { stm32_spi_irq_handler(4); }  /* SPI4 */
DMOD_IRQ_HANDLER(85) { stm32_spi_irq_handler(5); }  /* SPI5 */
DMOD_IRQ_HANDLER(86) { stm32_spi_irq_handler(6); }  /* SPI6 */
