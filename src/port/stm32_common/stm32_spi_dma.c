/* No DMOD_ENABLE_REGISTRATION here - see stm32_common.c. */
#include "dmod.h"
#include "dmosi.h"
#include "dmdma_lease.h"
#include "stm32_common.h"

/*
 * Master transfers of a few dozen bytes or more by DMA.
 *
 * A full-duplex transfer is two streams of the SPI's DMA controller (RM0385
 * table 26/27, RM0090 table 42/43): RX (peripheral -> memory) started first,
 * then TX (memory -> peripheral), then the SPI's RXDMAEN/TXDMAEN. The CPU
 * only waits for the RX stream's end on a semaphore - other threads run
 * while the bus is clocked, where the polled loop holds the CPU for the
 * whole transfer.
 *
 * Only when it can be done right; otherwise stm32_spi_dma_transfer() says so
 * (1) and the caller moves the bytes by itself:
 *  - the transfer is at least STM32_SPI_DMA_MIN_BYTES and less than 64 KiB;
 *  - the buffers are in memory the controller reaches (see
 *    stm32_spi_family_dma_reachable(): for DMA1 not the SDRAM) and start on and span whole
 *    D-cache lines (32 bytes): the cache is cleaned before the DMA reads a
 *    buffer and cleaned and invalidated before it writes one (so no dirty
 *    line is evicted over the incoming bytes) and invalidated again after
 *    it - maintenance never touches a neighbour's data. A missing buffer
 *    (tx: 0xFF are sent, rx: the bytes are dropped) needs none;
 *  - dmdma has the streams: they are leased at the first transfer and given
 *    back at deinit; a controller that is not there is not an error.
 */

#define DMA_MIN_BYTES       64U
#define DMA_MAX_BYTES       0xFFFFU
#define DMA_ALIGNMENT       32U
#define DMA_SLACK_MS        100U

typedef struct
{
    uint8_t controller;                 /* 0: DMA1, 1: DMA2 */
    uint8_t rx_stream[2];               /* The streams that can take the request, preferred first */
    uint8_t rx_channel[2];              /* ... and the request channel of each */
    uint8_t tx_stream[2];
    uint8_t tx_channel[2];
} spi_dma_map_t;

/* RM0385 table 26/27: the SPIs' requests (the same on STM32F4) */
static const spi_dma_map_t dma_map[STM32_SPI_MAX_INSTANCES] =
{
    /* SPI1 */ { 1, { 0, 2 }, { 3, 3 }, { 3, 5 }, { 3, 3 } },
    /* SPI2 */ { 0, { 3, 3 }, { 0, 0 }, { 4, 4 }, { 0, 0 } },
    /* SPI3 */ { 0, { 0, 2 }, { 0, 0 }, { 5, 7 }, { 0, 0 } },
    /* SPI4 */ { 1, { 0, 3 }, { 4, 5 }, { 1, 4 }, { 4, 5 } },
    /* SPI5 */ { 1, { 3, 5 }, { 2, 7 }, { 4, 6 }, { 2, 7 } },
    /* SPI6 */ { 1, { 6, 6 }, { 1, 1 }, { 5, 5 }, { 1, 1 } },
};

typedef struct
{
    dmdma_lease_t       rx;
    dmdma_lease_t       tx;
    uint8_t             rx_channel;
    uint8_t             tx_channel;
    dmosi_semaphore_t   done;
    volatile bool       active;         /* A transfer waits for its end: the callbacks count */
    volatile uint32_t   result;         /* 0 running, 1 complete, 2 failed */
    volatile uint32_t   last_event;     /* The failing one, for the log */
    bool                reported;       /* A failure was said already */
    bool                unavailable;    /* No streams: do not look again */
} spi_dma_t;

static spi_dma_t dma_state[STM32_SPI_MAX_INSTANCES];

/* A byte the DMA moves when there is no buffer: 0xFF out (what the polled
 * transfer sends), anything in (never read) */
static const uint8_t dummy_out = 0xFF;
static uint8_t dummy_in;

#define SCB_CCR             (*(volatile uint32_t *)0xE000ED14UL)
#define SCB_CCR_DC          (1UL << 16)
#define SCB_DCIMVAC         (*(volatile uint32_t *)0xE000EF5CUL)   /* invalidate */
#define SCB_DCCMVAC         (*(volatile uint32_t *)0xE000EF68UL)   /* clean */
#define SCB_DCCIMVAC        (*(volatile uint32_t *)0xE000EF70UL)   /* clean + invalidate */
#define DCACHE_LINE         32U

static void dcache_by_address(volatile uint32_t *operation, const void *buffer, uint32_t length)
{
    if ((SCB_CCR & SCB_CCR_DC) == 0U || length == 0U)
        return;
    uintptr_t line = (uintptr_t)buffer & ~(uintptr_t)(DCACHE_LINE - 1U);
    uintptr_t end  = (uintptr_t)buffer + length;
    __asm__ volatile ("dsb" ::: "memory");
    for (; line < end; line += DCACHE_LINE)
        *operation = (uint32_t)line;
    __asm__ volatile ("dsb\n\tisb" ::: "memory");
}

static bool buffer_usable(uint8_t controller, const void *buffer, size_t size)
{
    return buffer == NULL ||
           (((uintptr_t)buffer % DMA_ALIGNMENT) == 0U && (size % DMA_ALIGNMENT) == 0U &&
            stm32_spi_family_dma_reachable(controller, buffer, size));
}

/* The lease callbacks: interrupt context (or synchronous from _abort, then
 * !active) */
static void dma_callback(dmdma_lease_t lease, dmdma_event_t event, void *user_ptr)
{
    spi_dma_t *d = (spi_dma_t *)user_ptr;
    if (!d->active)
        return;

    bool rx = (lease == d->rx);
    if ((event & ~(dmdma_event_complete | dmdma_event_half_complete)) != 0)
    {
        /* An error, an abort or a timeout of either stream */
        d->last_event = (uint32_t)event | (rx ? 0x100U : 0x200U);
        d->result = 2U;
    }
    else if (rx && (event & dmdma_event_complete) != 0 && d->result == 0U)
    {
        d->result = 1U;
    }
    else
    {
        return;     /* TX done, a half: nothing to wake up for */
    }
    (void)dmosi_semaphore_post(d->done, 1);
}

static dmdma_lease_t acquire_one(const spi_dma_map_t *map, const uint8_t *stream, const uint8_t *channel,
                                 uint8_t *channel_out, spi_dma_t *d)
{
    for (unsigned i = 0; i < 2U; i++)
    {
        dmdma_lease_t lease = dmdma_lease_acquire(map->controller, stream[i]);
        if (lease == NULL)
            continue;
        if (dmdma_lease_set_callback(lease, dma_callback, d) != 0)
        {
            dmdma_lease_release(lease);
            continue;
        }
        *channel_out = channel[i];
        return lease;
    }
    return NULL;
}

static bool dma_ensure(unsigned idx)
{
    spi_dma_t *d = &dma_state[idx];
    if (d->rx != NULL && d->tx != NULL)
        return true;
    if (d->unavailable)
        return false;

    const spi_dma_map_t *map = &dma_map[idx];
    if (d->done == NULL)
        d->done = dmosi_semaphore_create(0, 1);
    if (d->rx == NULL)
        d->rx = acquire_one(map, map->rx_stream, map->rx_channel, &d->rx_channel, d);
    if (d->tx == NULL && d->rx != NULL)
        d->tx = acquire_one(map, map->tx_stream, map->tx_channel, &d->tx_channel, d);
    if (d->done == NULL || d->rx == NULL || d->tx == NULL)
    {
        DMOD_LOG_WARN("dmspi_port: SPI%u - no DMA streams (DMA%u): transfers by the CPU\n",
                      idx + 1U, (unsigned)map->controller + 1U);
        stm32_spi_dma_release(idx + 1U);
        d->unavailable = true;
        return false;
    }
    return true;
}

void stm32_spi_dma_release(dmspi_instance_t instance)
{
    spi_dma_t *d = &dma_state[instance - 1U];
    d->active = false;
    if (d->rx != NULL)
        dmdma_lease_release(d->rx);
    if (d->tx != NULL)
        dmdma_lease_release(d->tx);
    d->rx = d->tx = NULL;
    if (d->done != NULL)
        dmosi_semaphore_destroy(d->done);
    d->done = NULL;
    d->unavailable = false;
}

int stm32_spi_dma_transfer(dmspi_instance_t instance, volatile stm32_spi_t *SPI,
                           const uint8_t *tx, uint8_t *rx, size_t size, uint32_t clock_hz)
{
    unsigned idx = (unsigned)instance - 1U;
    uint8_t controller = dma_map[idx].controller;
    if (size < DMA_MIN_BYTES || size > DMA_MAX_BYTES ||
        !buffer_usable(controller, tx, size) || !buffer_usable(controller, rx, size))
        return 1;
    if (!dma_ensure(idx))
        return 1;

    spi_dma_t *d = &dma_state[idx];
    if (d->active)
        return -1;      /* Another transfer on this SPI */

    /* Longest the transfer can take: its bits at the clock the SPI runs at */
    uint32_t br   = (SPI->CR1 & STM32_SPI_CR1_BR_Msk) >> STM32_SPI_CR1_BR_Pos;
    uint32_t baud = clock_hz / (2U << br);
    uint32_t wait_ms = (baud != 0U ? (uint32_t)(((uint64_t)size * 8U * 1000U) / baud) : 0U) + DMA_SLACK_MS;

    (void)dmosi_semaphore_wait(d->done, 1, 0);      /* A late wake-up of an earlier transfer */
    d->result = 0;
    d->active = true;

    if (tx != NULL)
        dcache_by_address(&SCB_DCCMVAC, tx, (uint32_t)size);
    if (rx != NULL)
        dcache_by_address(&SCB_DCCIMVAC, rx, (uint32_t)size);

    dmdma_transfer_config_t rx_cfg = {
        .direction             = dmdma_direction_peripheral_to_memory,
        .request               = d->rx_channel,
        .source_address        = (const void *)(uintptr_t)&SPI->DR,
        .destination_address   = (rx != NULL) ? (void *)rx : (void *)&dummy_in,
        .source_width          = dmdma_data_width_byte,
        .destination_width     = dmdma_data_width_byte,
        .source_increment      = false,
        .destination_increment = (rx != NULL),
        .circular              = false,
        .priority              = dmdma_priority_high,
        .element_count         = size,
        /* No dmdma watchdog: it is a dmosi timer restarted on every start,
         * and many transfers a second fill the timer's command queue - the
         * start then blocks (its task has a low priority) with the bus
         * selected. The semaphore wait below bounds it instead. */
        .timeout_ms            = 0,
    };
    dmdma_transfer_config_t tx_cfg = {
        .direction             = dmdma_direction_memory_to_peripheral,
        .request               = d->tx_channel,
        .source_address        = (tx != NULL) ? (const void *)tx : (const void *)&dummy_out,
        .destination_address   = (void *)(uintptr_t)&SPI->DR,
        .source_width          = dmdma_data_width_byte,
        .destination_width     = dmdma_data_width_byte,
        .source_increment      = (tx != NULL),
        .destination_increment = false,
        .circular              = false,
        .priority              = dmdma_priority_medium,
        .element_count         = size,
        .timeout_ms            = 0,
    };

    int ret = -1;
    const char *why = "the RX stream did not start";
    if (dmdma_lease_start(d->rx, &rx_cfg) == 0)
    {
        why = "the TX stream did not start";
        if (dmdma_lease_start(d->tx, &tx_cfg) == 0)
        {
            SPI->CR2 |= STM32_SPI_CR2_RXDMAEN;
            SPI->CR2 |= STM32_SPI_CR2_TXDMAEN;
            if (dmosi_semaphore_wait(d->done, 1, (int32_t)(wait_ms + DMA_SLACK_MS)) != 0)
                why = "no end of the transfer in time";
            else if (d->result != 1U)
                why = "the DMA reported an error (event bits in last_event)";
            else
                ret = 0;
        }
    }

    /* The last bit out before the caller toggles chip select */
    uint32_t timeout = STM32_SPI_TIMEOUT_VALUE;
    while (ret == 0 && (SPI->SR & STM32_SPI_SR_BSY) != 0U && --timeout != 0U)
        ;
    SPI->CR2 &= ~(STM32_SPI_CR2_RXDMAEN | STM32_SPI_CR2_TXDMAEN);
    d->active = false;
    dmdma_lease_abort(d->tx);
    dmdma_lease_abort(d->rx);

    if (rx != NULL)
        dcache_by_address(&SCB_DCIMVAC, rx, (uint32_t)size);
    if (ret != 0 && !d->reported)
    {
        d->reported = true;
        DMOD_LOG_ERROR("dmspi_port: SPI%u DMA transfer of %u bytes failed: %s (event 0x%x, remaining %u, SR 0x%x)\n",
                       (unsigned)instance, (unsigned)size, why, (unsigned)d->last_event,
                       (unsigned)dmdma_lease_get_remaining(d->rx), (unsigned)SPI->SR);
    }
    if ((SPI->SR & (STM32_SPI_SR_OVR | STM32_SPI_SR_MODF)) != 0U)
    {
        (void)SPI->DR;
        (void)SPI->SR;
        SPI->CR1 |= STM32_SPI_CR1_SPE;
        ret = -1;
    }
    return ret;
}
