/* DMOD_ENABLE_REGISTRATION is intentionally NOT set here: each family's
 * port.c (which includes dmspi_port.h with the flag on) is the single
 * translation unit that emits this module's API registration table.
 * Defining it here too would duplicate every registration entry at link
 * time, since this file provides the actual function bodies that table
 * points to. See dmgpio/src/port/stm32_common/stm32_common.c for the same
 * split. */
#include "dmod.h"
#include "dmosi.h"
#include "dmclk_port.h"
#include "dm_sw_ring.h"
#include "stm32_common.h"

#define STM32_APB1PERIPH_BASE    0x40000000UL
#define STM32_APB2PERIPH_BASE    0x40010000UL

/* Instance descriptor table - identical across STM32F4/F7 for every
 * instance the two families share (base address, RCC bus/bit and IRQ
 * number assignment are the same on both). Boards that don't wire up the
 * higher-numbered instances simply never configure them. */
const stm32_spi_instance_desc_t stm32_spi_instances[STM32_SPI_MAX_INSTANCES] =
{
    /* SPI1 */ { STM32_APB2PERIPH_BASE + 0x3000UL, &STM32_RCC->APB2ENR, 12U, STM32_RCC_CFGR_PPRE2_Pos, 35U },
    /* SPI2 */ { STM32_APB1PERIPH_BASE + 0x3800UL, &STM32_RCC->APB1ENR, 14U, STM32_RCC_CFGR_PPRE1_Pos, 36U },
    /* SPI3 */ { STM32_APB1PERIPH_BASE + 0x3C00UL, &STM32_RCC->APB1ENR, 15U, STM32_RCC_CFGR_PPRE1_Pos, 51U },
    /* SPI4 */ { STM32_APB2PERIPH_BASE + 0x3400UL, &STM32_RCC->APB2ENR, 13U, STM32_RCC_CFGR_PPRE2_Pos, 84U },
    /* SPI5 */ { STM32_APB2PERIPH_BASE + 0x5000UL, &STM32_RCC->APB2ENR, 20U, STM32_RCC_CFGR_PPRE2_Pos, 85U },
    /* SPI6 */ { STM32_APB2PERIPH_BASE + 0x5400UL, &STM32_RCC->APB2ENR, 21U, STM32_RCC_CFGR_PPRE2_Pos, 86U },
};

/* Per-instance interrupt handler registration */
static dmspi_port_interrupt_handler_t irq_handlers[STM32_SPI_MAX_INSTANCES];
static void *irq_user_ptrs[STM32_SPI_MAX_INSTANCES];

/* Per-instance SW ring buffers - set via dmspi_port_set_rx_ring() */
static dm_sw_ring_t rx_rings[STM32_SPI_MAX_INSTANCES];

void stm32_spi_common_init(void)
{
    for (unsigned i = 0; i < STM32_SPI_MAX_INSTANCES; i++)
    {
        irq_handlers[i]  = NULL;
        irq_user_ptrs[i] = NULL;
        rx_rings[i]      = NULL;
    }
}

void stm32_spi_common_deinit(void)
{
    /* Nothing to release: rings are owned/destroyed by the core dmspi driver */
}

void stm32_spi_nvic_enable_irq(uint32_t irqn)
{
    /* NVIC priority defaults to 0 (highest, non-maskable by FreeRTOS critical
     * sections) after reset. Any interrupt that may call dmosi/FreeRTOS
     * ISR-safe API (as this one does, via dm_sw_ring signaling a semaphore
     * from within the IRQ handler) must be configured at or below the
     * priority dmosi_get_min_interrupt_priority() reports, or the RTOS
     * cannot safely mask it during its own critical sections. */
    volatile uint8_t  *nvic_ip   = (volatile uint8_t *)0xE000E400UL;
    volatile uint32_t *nvic_iser = (volatile uint32_t *)0xE000E100UL;
    nvic_ip[irqn] = (uint8_t)dmosi_get_min_interrupt_priority();
    nvic_iser[irqn >> 5U] = 1U << (irqn & 0x1FU);
}

void stm32_spi_nvic_disable_irq(uint32_t irqn)
{
    volatile uint32_t *nvic_icer = (volatile uint32_t *)0xE000E180UL;
    nvic_icer[irqn >> 5U] = 1U << (irqn & 0x1FU);
}

static int validate_instance(dmspi_instance_t instance)
{
    return (instance >= 1 && instance <= STM32_SPI_MAX_INSTANCES) ? 0 : -1;
}

static volatile stm32_spi_t *get_spi(dmspi_instance_t instance)
{
    return (volatile stm32_spi_t *)stm32_spi_instances[instance - 1].base;
}

static void enable_spi_clock(dmspi_instance_t instance)
{
    const stm32_spi_instance_desc_t *desc = &stm32_spi_instances[instance - 1];
    *desc->enr |= (1U << desc->enable_bit);
}

/* Convert a 3-bit RCC_CFGR PPREx field to its division factor.
 * Encoding (same on STM32F4/F7): MSB=0 -> not divided (/1);
 * 100->/2, 101->/4, 110->/8, 111->/16. */
static uint32_t apb_prescaler_div(uint32_t ppre_bits)
{
    if ((ppre_bits & 0x4U) == 0U) return 1U;
    return 2U << (ppre_bits & 0x3U);
}

static uint32_t get_spi_clock(dmspi_instance_t instance)
{
    const stm32_spi_instance_desc_t *desc = &stm32_spi_instances[instance - 1];

    /* Query the real, currently-configured core clock from dmclk instead of
     * assuming the reset-default HSI - dmclk may have switched to a PLL/HSE
     * derived frequency before this module is configured. */
    uint32_t sysclk = (uint32_t)dmclk_port_get_current_frequency();
    if (sysclk == 0U)
        sysclk = STM32_HSI_VALUE; /* defensive fallback, should not normally happen */

    uint32_t ppre = (STM32_RCC->CFGR >> desc->ppre_shift) & 0x7U;
    return sysclk / apb_prescaler_div(ppre);
}

/* ---- Port API implementation ---- */

dmod_dmspi_port_api_declaration(1.0, int, _init, ( dmspi_instance_t instance, dmspi_role_t role ))
{
    if (validate_instance(instance) != 0) return -1;

    enable_spi_clock(instance);

    volatile stm32_spi_t *SPI = get_spi(instance);

    SPI->CR1 = 0;
    SPI->CR2 = 0;
    stm32_spi_family_configure_frame_format(SPI);

    /* Default to software NSS management until dmspi_port_set_nss_mode()
     * runs; SSI defaults to "selected" for a slave and "deselected" for a
     * master so neither role raises a spurious mode fault before the rest
     * of the configuration sequence completes. */
    SPI->CR1 |= STM32_SPI_CR1_SSM;
    if (role == dmspi_role_master)
        SPI->CR1 |= STM32_SPI_CR1_MSTR | STM32_SPI_CR1_SSI;

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _deinit, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;
    SPI->CR2 = 0;

    stm32_spi_nvic_disable_irq(stm32_spi_instances[instance - 1].irqn);

    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _transfer, ( dmspi_instance_t instance, const uint8_t* tx, uint8_t* rx, size_t size ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);

    for (size_t i = 0; i < size; i++)
    {
        uint32_t timeout = STM32_SPI_TIMEOUT_VALUE;
        while (!(SPI->SR & STM32_SPI_SR_TXE))
        {
            if (--timeout == 0) return -1;
        }
        SPI->DR = tx ? tx[i] : 0xFFU;

        timeout = STM32_SPI_TIMEOUT_VALUE;
        while (!(SPI->SR & STM32_SPI_SR_RXNE))
        {
            if (--timeout == 0) return -1;
        }

        uint32_t sr = SPI->SR;
        uint8_t byte = (uint8_t)(SPI->DR & 0xFFU); /* reading DR completes the OVR clear sequence too */

        if (sr & (STM32_SPI_SR_OVR | STM32_SPI_SR_MODF))
        {
            /* MODF clear sequence is: read SR (done above), then write CR1 -
             * re-assert SPE so the peripheral keeps running afterwards. */
            SPI->CR1 |= STM32_SPI_CR1_SPE;
            return -1;
        }

        if (rx != NULL) rx[i] = byte;
    }

    /* Wait for the last bit to finish shifting out before returning, so a
     * caller that immediately toggles chip-select does not cut it short. */
    uint32_t timeout = STM32_SPI_TIMEOUT_VALUE;
    while (SPI->SR & STM32_SPI_SR_BSY)
    {
        if (--timeout == 0) return -1;
    }

    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _transmit, ( dmspi_instance_t instance, const uint8_t* data, size_t size ))
{
    if (data == NULL) return -1;
    return dmspi_port_transfer(instance, data, NULL, size);
}

dmod_dmspi_port_api_declaration(1.0, int, _receive, ( dmspi_instance_t instance, uint8_t* data, size_t size, size_t* received ))
{
    if (validate_instance(instance) != 0 || data == NULL || received == NULL) return -1;

    dm_sw_ring_t ring = rx_rings[instance - 1];
    if (ring != NULL)
    {
        *received = (size_t)dm_sw_ring_read(ring, data, (dm_sw_ring_capacity_t)size);
        return 0;
    }

    volatile stm32_spi_t *SPI = get_spi(instance);
    *received = 0;

    for (size_t i = 0; i < size; i++)
    {
        uint32_t timeout = STM32_SPI_TIMEOUT_VALUE;
        while (!(SPI->SR & STM32_SPI_SR_TXE))
        {
            if (--timeout == 0) return 0;
        }
        SPI->DR = 0xFFU; /* dummy byte - keeps the clock/shift register moving */

        timeout = STM32_SPI_TIMEOUT_VALUE;
        while (!(SPI->SR & STM32_SPI_SR_RXNE))
        {
            if (--timeout == 0) return 0;
        }

        uint32_t sr = SPI->SR;
        data[i] = (uint8_t)(SPI->DR & 0xFFU);

        if (sr & (STM32_SPI_SR_OVR | STM32_SPI_SR_MODF))
        {
            SPI->CR1 |= STM32_SPI_CR1_SPE;
            return -1;
        }
        (*received)++;
    }

    return 0;
}

dmod_dmspi_port_api_declaration(1.0, bool, _is_busy, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return false;
    return (get_spi(instance)->SR & STM32_SPI_SR_BSY) != 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_role, ( dmspi_instance_t instance, dmspi_role_t role ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;

    if (role == dmspi_role_master)
        SPI->CR1 |= STM32_SPI_CR1_MSTR;
    else
        SPI->CR1 &= ~STM32_SPI_CR1_MSTR;

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_role_t, _get_role, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_role_slave;
    return (get_spi(instance)->CR1 & STM32_SPI_CR1_MSTR) ? dmspi_role_master : dmspi_role_slave;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_baudrate, ( dmspi_instance_t instance, dmspi_baudrate_t baudrate ))
{
    if (validate_instance(instance) != 0 || baudrate == 0) return -1;

    uint32_t pclk = get_spi_clock(instance);

    /* BR[2:0] selects a /2../256 divisor; pick the smallest divisor whose
     * resulting SCK does not exceed the requested rate. */
    uint32_t br = 0;
    while (br < 7U && (pclk / (2U << br)) > baudrate)
        br++;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;
    SPI->CR1 = (SPI->CR1 & ~STM32_SPI_CR1_BR_Msk) | (br << STM32_SPI_CR1_BR_Pos);
    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_baudrate_t, _get_baudrate, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return 0;

    uint32_t pclk = get_spi_clock(instance);
    uint32_t br = (get_spi(instance)->CR1 & STM32_SPI_CR1_BR_Msk) >> STM32_SPI_CR1_BR_Pos;
    return (dmspi_baudrate_t)(pclk / (2U << br));
}

dmod_dmspi_port_api_declaration(1.0, int, _set_clock_polarity, ( dmspi_instance_t instance, dmspi_clock_polarity_t polarity ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;

    if (polarity == dmspi_clock_polarity_high)
        SPI->CR1 |= STM32_SPI_CR1_CPOL;
    else
        SPI->CR1 &= ~STM32_SPI_CR1_CPOL;

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_clock_polarity_t, _get_clock_polarity, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_clock_polarity_low;
    return (get_spi(instance)->CR1 & STM32_SPI_CR1_CPOL) ? dmspi_clock_polarity_high : dmspi_clock_polarity_low;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_clock_phase, ( dmspi_instance_t instance, dmspi_clock_phase_t phase ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;

    if (phase == dmspi_clock_phase_2_edge)
        SPI->CR1 |= STM32_SPI_CR1_CPHA;
    else
        SPI->CR1 &= ~STM32_SPI_CR1_CPHA;

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_clock_phase_t, _get_clock_phase, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_clock_phase_1_edge;
    return (get_spi(instance)->CR1 & STM32_SPI_CR1_CPHA) ? dmspi_clock_phase_2_edge : dmspi_clock_phase_1_edge;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_bit_order, ( dmspi_instance_t instance, dmspi_bit_order_t bit_order ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    SPI->CR1 &= ~STM32_SPI_CR1_SPE;

    if (bit_order == dmspi_bit_order_lsb_first)
        SPI->CR1 |= STM32_SPI_CR1_LSBFIRST;
    else
        SPI->CR1 &= ~STM32_SPI_CR1_LSBFIRST;

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_bit_order_t, _get_bit_order, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_bit_order_msb_first;
    return (get_spi(instance)->CR1 & STM32_SPI_CR1_LSBFIRST) ? dmspi_bit_order_lsb_first : dmspi_bit_order_msb_first;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_nss_mode, ( dmspi_instance_t instance, dmspi_nss_mode_t nss_mode ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);
    bool is_master = (SPI->CR1 & STM32_SPI_CR1_MSTR) != 0;

    SPI->CR1 &= ~STM32_SPI_CR1_SPE;

    if (nss_mode == dmspi_nss_mode_soft)
    {
        SPI->CR1 |= STM32_SPI_CR1_SSM;
        /* SSI feeds the internal NSS signal when SSM=1: a master must see it
         * high (deselected - CS is managed externally, e.g. via dmgpio) to
         * avoid a spurious mode fault; a slave must see it low (permanently
         * selected) since there is no external NSS pin to gate it. */
        if (is_master)
            SPI->CR1 |= STM32_SPI_CR1_SSI;
        else
            SPI->CR1 &= ~STM32_SPI_CR1_SSI;
        SPI->CR2 &= ~STM32_SPI_CR2_SSOE;
    }
    else
    {
        SPI->CR1 &= ~(STM32_SPI_CR1_SSM | STM32_SPI_CR1_SSI);
        /* SSOE lets a master drive the physical NSS pin automatically
         * (single-slave systems only); a slave always relies on the
         * physical pin once SSM=0, so SSOE is left clear for it. */
        if (is_master)
            SPI->CR2 |= STM32_SPI_CR2_SSOE;
        else
            SPI->CR2 &= ~STM32_SPI_CR2_SSOE;
    }

    SPI->CR1 |= STM32_SPI_CR1_SPE;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_nss_mode_t, _get_nss_mode, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_nss_mode_soft;
    return (get_spi(instance)->CR1 & STM32_SPI_CR1_SSM) ? dmspi_nss_mode_soft : dmspi_nss_mode_hard;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_interrupt_trigger, ( dmspi_instance_t instance, dmspi_int_trigger_t trigger ))
{
    if (validate_instance(instance) != 0) return -1;

    volatile stm32_spi_t *SPI = get_spi(instance);

    SPI->CR2 &= ~(STM32_SPI_CR2_RXNEIE | STM32_SPI_CR2_TXEIE | STM32_SPI_CR2_ERRIE);

    if (trigger & dmspi_int_trigger_rx_not_empty) SPI->CR2 |= STM32_SPI_CR2_RXNEIE;
    if (trigger & dmspi_int_trigger_tx_empty)     SPI->CR2 |= STM32_SPI_CR2_TXEIE;
    if (trigger & dmspi_int_trigger_error)        SPI->CR2 |= STM32_SPI_CR2_ERRIE;

    uint32_t irqn = stm32_spi_instances[instance - 1].irqn;
    if (trigger != dmspi_int_trigger_off)
        stm32_spi_nvic_enable_irq(irqn);
    else
        stm32_spi_nvic_disable_irq(irqn);

    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _read_interrupt_trigger, ( dmspi_instance_t instance, dmspi_int_trigger_t *out_trigger ))
{
    if (validate_instance(instance) != 0 || out_trigger == NULL) return -1;

    uint32_t cr2 = get_spi(instance)->CR2;
    *out_trigger = dmspi_int_trigger_off;

    if (cr2 & STM32_SPI_CR2_RXNEIE) *out_trigger = (dmspi_int_trigger_t)(*out_trigger | dmspi_int_trigger_rx_not_empty);
    if (cr2 & STM32_SPI_CR2_TXEIE)  *out_trigger = (dmspi_int_trigger_t)(*out_trigger | dmspi_int_trigger_tx_empty);
    if (cr2 & STM32_SPI_CR2_ERRIE)  *out_trigger = (dmspi_int_trigger_t)(*out_trigger | dmspi_int_trigger_error);

    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _add_interrupt_handler,
    ( dmspi_instance_t instance, dmspi_port_interrupt_handler_t handler, void *user_ptr ))
{
    if (validate_instance(instance) != 0 || handler == NULL) return -1;

    irq_handlers[instance - 1]  = handler;
    irq_user_ptrs[instance - 1] = user_ptr;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _remove_interrupt_handler,
    ( dmspi_instance_t instance, void *user_ptr ))
{
    if (validate_instance(instance) != 0) return -1;

    if (irq_user_ptrs[instance - 1] == user_ptr)
    {
        irq_handlers[instance - 1]  = NULL;
        irq_user_ptrs[instance - 1] = NULL;
    }
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_rx_ring,
    ( dmspi_instance_t instance, dm_sw_ring_t ring ))
{
    if (validate_instance(instance) != 0) return -1;
    rx_rings[instance - 1] = ring;
    return 0;
}

/* ---- ISR handler, shared by every family's DMOD_IRQ_HANDLER ---- */

void stm32_spi_irq_handler(dmspi_instance_t instance)
{
    if (validate_instance(instance) != 0) return;

    volatile stm32_spi_t *SPI = get_spi(instance);
    uint32_t idx = (uint32_t)(instance - 1);

    uint8_t data = 0;
    dmspi_int_trigger_t trigger = dmspi_int_trigger_off;
    uint32_t sr = SPI->SR;

    if (sr & STM32_SPI_SR_RXNE)
    {
        data = (uint8_t)(SPI->DR & 0xFFU); /* reading DR clears RXNE (and completes an OVR clear) */
        trigger = (dmspi_int_trigger_t)(trigger | dmspi_int_trigger_rx_not_empty);

        if (rx_rings[idx] != NULL)
            dm_sw_ring_write(rx_rings[idx], &data, 1);

        sr = SPI->SR;
    }
    if (sr & STM32_SPI_SR_TXE)
        trigger = (dmspi_int_trigger_t)(trigger | dmspi_int_trigger_tx_empty);
    if (sr & (STM32_SPI_SR_OVR | STM32_SPI_SR_MODF))
    {
        (void)SPI->DR;                   /* completes the OVR clear sequence */
        SPI->CR1 |= STM32_SPI_CR1_SPE;   /* completes the MODF clear sequence */
        trigger = (dmspi_int_trigger_t)(trigger | dmspi_int_trigger_error);
    }

    dmspi_port_interrupt_handler_t handler = irq_handlers[idx];
    if (handler != NULL && trigger != dmspi_int_trigger_off)
        handler(irq_user_ptrs[idx], instance, trigger, data);
}
