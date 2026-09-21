#define DMOD_ENABLE_REGISTRATION    ON
#include "dmspi_port.h"
#include "dmod.h"
#include "../stm32_common/stm32_common.h"

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    Dmod_Printf("dmspi port module initialized (stm32f4)\n");
    stm32_spi_common_init();
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmspi port module deinitialized (stm32f4)\n");
    stm32_spi_common_deinit();
    return 0;
}

/* ---- Family-specific register quirk ----
 *
 * STM32F4 implements the classic SPI IP: data size is selected via
 * CR1.DFF (0 = 8-bit, 1 = 16-bit) and RXNE always asserts per-byte, so
 * this driver's 8-bit-only operation just needs DFF defensively cleared -
 * there is no FIFO/threshold to configure like on F7. */
void stm32_spi_family_configure_frame_format(volatile stm32_spi_t *SPI)
{
    SPI->CR1 &= ~STM32_SPI_CR1_DFF;
}

/* ---- IRQ handlers ---- */

DMOD_IRQ_HANDLER(35) { stm32_spi_irq_handler(1); }  /* SPI1 */
DMOD_IRQ_HANDLER(36) { stm32_spi_irq_handler(2); }  /* SPI2 */
DMOD_IRQ_HANDLER(51) { stm32_spi_irq_handler(3); }  /* SPI3 */
DMOD_IRQ_HANDLER(84) { stm32_spi_irq_handler(4); }  /* SPI4 */
DMOD_IRQ_HANDLER(85) { stm32_spi_irq_handler(5); }  /* SPI5 */
DMOD_IRQ_HANDLER(86) { stm32_spi_irq_handler(6); }  /* SPI6 */
