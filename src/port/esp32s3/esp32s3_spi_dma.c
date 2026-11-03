/* No DMOD_ENABLE_REGISTRATION here - see port.c. */
#include "dmod.h"
#include "dmosi.h"
#include <string.h>
#include "dmdma_lease.h"
#include "esp32s3_spi.h"
#include "esp32s3_spi_dma.h"

/*
 * Master transfers by the GDMA (dmdma port for the ESP32-S3): a full-duplex transfer is two streams of the GDMA, RX (peripheral ->
 * memory) started first, then TX (memory -> peripheral), then the transaction of the SPI. The CPU only waits on a semaphore for the
 * end of the RX stream - other threads run while the bus is clocked, where the polled transactions hold the CPU for the whole transfer.
 *
 * The GDMA reaches the internal RAM only, and the heap puts buffers in the external RAM first: a buffer that is not in the internal
 * RAM goes through a bounce buffer of the heap tagged "dma" (4092 bytes each way, the CPU copies a chunk in or out), one that is, is
 * moved in place (as long as it is a multiple of 4 bytes where the SPI writes: the controller writes whole words). The streams and
 * the bounce buffers are taken at the first transfer and given back at deinit; when they are not available the polled path is used.
 */

#define BOUNCE_BYTES        4092U               /* one descriptor; a multiple of 4 */
#define DMA_SLACK_MS        100U
#define GDMA_CONTROLLER     0U
#define GDMA_STREAMS        5U
#define INTERNAL_RAM_START  0x3FC88000UL
#define INTERNAL_RAM_END    0x3FCF0000UL

typedef struct
{
    dmdma_lease_t       rx;
    dmdma_lease_t       tx;
    dmosi_semaphore_t   done;
    uint8_t            *tx_buf;
    uint8_t            *rx_buf;
    volatile bool       active;
    volatile uint32_t   result;                 /* 0 running, 1 complete, 2 failed */
    volatile uint32_t   last_event;
    bool                unavailable;
    bool                reported;
} spi_dma_t;

static spi_dma_t dma_state[ESP32S3_SPI_COUNT];

static inline volatile uint32_t *creg(unsigned index, uint32_t offset)
{
    return (volatile uint32_t *)(ESP32S3_SPI_BASE(index) + offset);
}

static bool internal_ram(const void *buffer, size_t size)
{
    uintptr_t start = (uintptr_t)buffer;
    return start >= INTERNAL_RAM_START && start < INTERNAL_RAM_END && size <= INTERNAL_RAM_END - start;
}

/* The lease callbacks: interrupt context (or synchronous from _abort, then !active) */
static void dma_callback(dmdma_lease_t lease, dmdma_event_t event, void *user_ptr)
{
    spi_dma_t *d = (spi_dma_t *)user_ptr;
    if (!d->active)
        return;

    bool rx = (lease == d->rx);
    if ((event & ~(dmdma_event_complete | dmdma_event_half_complete)) != 0)
    {
        d->last_event = (uint32_t)event | (rx ? 0x100U : 0x200U);
        d->result = 2U;
    }
    else if (rx && (event & dmdma_event_complete) != 0 && d->result == 0U)
    {
        d->result = 1U;
    }
    else
    {
        return;                                 /* TX done, a half: nothing to wake up for */
    }
    (void)dmosi_semaphore_post(d->done, 1);
}

/* The first free stream from `first` on (the number of the one taken is in *taken: the next search goes past it) */
static dmdma_lease_t acquire_stream(spi_dma_t *d, uint8_t first, uint8_t *taken)
{
    for (uint8_t stream = first; stream < GDMA_STREAMS; stream++)
    {
        dmdma_lease_t lease = dmdma_lease_acquire(GDMA_CONTROLLER, stream);
        if (lease == NULL)
            continue;
        if (dmdma_lease_set_callback(lease, dma_callback, d) == 0)
        {
            *taken = stream;
            return lease;
        }
        dmdma_lease_release(lease);
    }
    return NULL;
}

static bool dma_ensure(unsigned index)
{
    spi_dma_t *d = &dma_state[index];
    if (d->rx != NULL && d->tx != NULL && d->tx_buf != NULL && d->rx_buf != NULL)
        return true;
    if (d->unavailable)
        return false;

    if (d->done == NULL)
        d->done = dmosi_semaphore_create(0, 1);
    uint8_t rx_stream = 0, tx_stream = 0;
    if (d->rx == NULL)
        d->rx = acquire_stream(d, 0, &rx_stream);
    if (d->tx == NULL && d->rx != NULL)
        d->tx = acquire_stream(d, (uint8_t)(rx_stream + 1U), &tx_stream);
    if (d->tx_buf == NULL)
        d->tx_buf = Dmod_MallocTagged("dma", BOUNCE_BYTES);
    if (d->rx_buf == NULL)
        d->rx_buf = Dmod_MallocTagged("dma", BOUNCE_BYTES);
    if (d->done == NULL || d->rx == NULL || d->tx == NULL || d->tx_buf == NULL || d->rx_buf == NULL ||
        !internal_ram(d->tx_buf, BOUNCE_BYTES) || !internal_ram(d->rx_buf, BOUNCE_BYTES))
    {
        DMOD_LOG_WARN("dmspi_port: SPI%u - no DMA streams or buffers: transfers by the CPU\n", index + 2U);
        esp32s3_spi_dma_release(index);
        d->unavailable = true;
        return false;
    }
    return true;
}

void esp32s3_spi_dma_release(unsigned index)
{
    spi_dma_t *d = &dma_state[index];
    d->active = false;
    if (d->rx != NULL)
        dmdma_lease_release(d->rx);
    if (d->tx != NULL)
        dmdma_lease_release(d->tx);
    d->rx = d->tx = NULL;
    if (d->done != NULL)
        dmosi_semaphore_destroy(d->done);
    d->done = NULL;
    if (d->tx_buf != NULL)
        Dmod_Free(d->tx_buf);
    if (d->rx_buf != NULL)
        Dmod_Free(d->rx_buf);
    d->tx_buf = d->rx_buf = NULL;
    d->unavailable = false;
}

static void update(unsigned index)
{
    uint32_t guard = 100000U;
    *creg(index, SPI_CMD_REG) |= SPI_CMD_UPDATE;
    while ((*creg(index, SPI_CMD_REG) & SPI_CMD_UPDATE) && --guard != 0U) { }
}

/* One transaction of `n` bytes between internal-RAM buffers (the RX one is a multiple of 4 bytes long) */
static int run_chunk(unsigned index, spi_dma_t *d, const uint8_t *txp, uint8_t *rxp, size_t n, size_t rx_n, uint32_t baudrate)
{
    uint32_t wait_ms = (baudrate != 0U ? (uint32_t)(((uint64_t)n * 8U * 1000U) / baudrate) : 0U) + DMA_SLACK_MS;

    (void)dmosi_semaphore_wait(d->done, 1, 0);          /* a late wake-up of an earlier transfer */
    d->result = 0;
    d->active = true;

    *creg(index, SPI_DMA_CONF_REG) &= ~(SPI_DMA_CONF_DMA_RX_ENA | SPI_DMA_CONF_DMA_TX_ENA | SPI_DMA_CONF_RX_EOF_EN);
    *creg(index, SPI_DMA_CONF_REG) |= SPI_DMA_CONF_DMA_AFIFO_RST | SPI_DMA_CONF_RX_AFIFO_RST | SPI_DMA_CONF_BUF_AFIFO_RST;
    *creg(index, SPI_DMA_CONF_REG) &= ~(SPI_DMA_CONF_DMA_AFIFO_RST | SPI_DMA_CONF_RX_AFIFO_RST | SPI_DMA_CONF_BUF_AFIFO_RST);
    *creg(index, SPI_MS_DLEN_REG) = (uint32_t)n * 8U - 1U;
    *creg(index, SPI_DMA_INT_CLR_REG) = SPI_DMA_INT_TRANS_DONE;
    *creg(index, SPI_DMA_CONF_REG) |= SPI_DMA_CONF_DMA_RX_ENA | SPI_DMA_CONF_DMA_TX_ENA;
    update(index);

    dmdma_transfer_config_t rx_cfg = {
        .direction             = dmdma_direction_peripheral_to_memory,
        .request               = (dmdma_request_t)index,        /* PERI_SEL of the GDMA: SPI2 0, SPI3 1 */
        .source_address        = (const void *)(uintptr_t)(ESP32S3_SPI_BASE(index) + SPI_W0_REG),  /* ignored: the GDMA is wired to the SPI */
        .destination_address   = rxp,
        .source_width          = dmdma_data_width_byte,
        .destination_width     = dmdma_data_width_byte,
        .source_increment      = false,
        .destination_increment = true,
        .circular              = false,
        .priority              = dmdma_priority_high,
        .element_count         = rx_n,
        .timeout_ms            = 0,
    };
    dmdma_transfer_config_t tx_cfg = {
        .direction             = dmdma_direction_memory_to_peripheral,
        .request               = (dmdma_request_t)index,
        .source_address        = txp,
        .destination_address   = (void *)(uintptr_t)(ESP32S3_SPI_BASE(index) + SPI_W0_REG),
        .source_width          = dmdma_data_width_byte,
        .destination_width     = dmdma_data_width_byte,
        .source_increment      = true,
        .destination_increment = false,
        .circular              = false,
        .priority              = dmdma_priority_medium,
        .element_count         = n,
        .timeout_ms            = 0,
    };

    int ret = -1;
    const char *why = "the RX stream did not start";
    if (dmdma_lease_start(d->rx, &rx_cfg) == 0)
    {
        why = "the TX stream did not start";
        if (dmdma_lease_start(d->tx, &tx_cfg) == 0)
        {
            *creg(index, SPI_CMD_REG) |= SPI_CMD_USR;
            if (dmosi_semaphore_wait(d->done, 1, (int32_t)wait_ms) != 0)
                why = "no end of the transfer in time";
            else if (d->result != 1U)
                why = "the DMA reported an error (event bits in last_event)";
            else
                ret = 0;
        }
    }

    /* The last bit out before the caller toggles chip select */
    uint64_t start = Dmod_GetUptime();
    while (ret == 0 && !(*creg(index, SPI_DMA_INT_RAW_REG) & SPI_DMA_INT_TRANS_DONE) && Dmod_GetUptime() - start < wait_ms)
        ;
    *creg(index, SPI_DMA_INT_CLR_REG) = SPI_DMA_INT_TRANS_DONE;
    *creg(index, SPI_DMA_CONF_REG) &= ~(SPI_DMA_CONF_DMA_RX_ENA | SPI_DMA_CONF_DMA_TX_ENA);
    d->active = false;
    dmdma_lease_abort(d->tx);
    dmdma_lease_abort(d->rx);

    if (ret != 0 && !d->reported)
    {
        d->reported = true;
        DMOD_LOG_ERROR("dmspi_port: SPI%u DMA transfer of %u bytes failed: %s (event 0x%x, remaining %u)\n", index + 2U,
                       (unsigned)n, why, (unsigned)d->last_event, (unsigned)dmdma_lease_get_remaining(d->rx));
    }
    return ret;
}

int esp32s3_spi_dma_transfer(unsigned index, const uint8_t *tx, uint8_t *rx, size_t size, uint32_t baudrate)
{
    if (index >= ESP32S3_SPI_COUNT || size < ESP32S3_SPI_DMA_MIN_BYTES || !dma_ensure(index))
        return 1;
    spi_dma_t *d = &dma_state[index];
    if (d->active)
        return -1;                                      /* another transfer on this controller */

    for (size_t done = 0; done < size;)
    {
        size_t n = size - done < BOUNCE_BYTES ? size - done : BOUNCE_BYTES;
        const uint8_t *txp;
        uint8_t *rxp;
        size_t rx_n = n;

        if (tx != NULL && internal_ram(tx + done, n))
        {
            txp = tx + done;
        }
        else
        {
            if (tx != NULL)
                memcpy(d->tx_buf, tx + done, n);
            else
                memset(d->tx_buf, 0xFF, n);
            txp = d->tx_buf;
        }
        if (rx != NULL && internal_ram(rx + done, n) && (n % 4U) == 0U)
        {
            rxp = rx + done;
        }
        else
        {
            rxp = d->rx_buf;
            rx_n = (n + 3U) & ~(size_t)3U;              /* the controller writes whole words */
        }

        if (run_chunk(index, d, txp, rxp, n, rx_n, baudrate) != 0)
            return -1;
        if (rx != NULL && rxp == d->rx_buf)
            memcpy(rx + done, d->rx_buf, n);
        done += n;
    }
    return 0;
}
