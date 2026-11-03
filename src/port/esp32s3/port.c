#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmspi_port.h"
#include "esp32s3_spi.h"
#include <errno.h>
#include <string.h>

/*
 * dmspi_port for the ESP32-S3: the master of the general purpose SPI controllers GP-SPI2 and GP-SPI3 (instances 2 and 3).
 *
 * A transfer is cut into transactions of 64 bytes (the data buffer W0..W15 of the controller): the bytes are put into the buffer, the
 * transaction is started and waited for (polling), the received ones are taken from the same buffer. Full duplex always; _receive shifts
 * out 0xFF, _transmit drops what comes in. No DMA, no interrupts, no slave role (-ENOTSUP / -1).
 *
 * The signals go to the pads through the GPIO matrix (dmspi_port_set_pins; the keys sck_pin, mosi_pin, miso_pin, cs_pin). nss_mode=hard
 * makes the controller drive CS0 on cs_pin, low during a transaction - and so it rises between two transactions of a long transfer;
 * for frames longer than 64 bytes use nss_mode=soft and a GPIO as the chip select (the friend of dmspi).
 */

typedef struct
{
    bool                    initialised;
    int                     sck, mosi, miso, cs;
    dmspi_baudrate_t        baudrate;
    dmspi_clock_polarity_t  polarity;
    dmspi_clock_phase_t     phase;
    dmspi_bit_order_t       bit_order;
    dmspi_nss_mode_t        nss_mode;
    dmspi_int_trigger_t     trigger;
} spi_t;

static spi_t controllers[ESP32S3_SPI_COUNT] = { { .sck = -1, .mosi = -1, .miso = -1, .cs = -1 }, { .sck = -1, .mosi = -1, .miso = -1, .cs = -1 } };

static inline volatile uint32_t *reg(uintptr_t address)
{
    return (volatile uint32_t *)address;
}

static inline volatile uint32_t *creg(unsigned index, uint32_t offset)
{
    return reg(ESP32S3_SPI_BASE(index) + offset);
}

static int index_of(dmspi_instance_t instance)
{
    return (instance >= ESP32S3_SPI_FIRST_INSTANCE && instance < ESP32S3_SPI_FIRST_INSTANCE + ESP32S3_SPI_COUNT)
               ? (int)(instance - ESP32S3_SPI_FIRST_INSTANCE) : -1;
}

/* GPIO0-21 and GPIO38-48; GPIO19/20 (USB) and GPIO26-37 (the flash and the PSRAM of the module) are not for the user */
static bool pad_valid(int pad)
{
    return pad < 0 || (pad >= 0 && pad <= 18) || pad == 21 || (pad >= 38 && pad <= 48);
}

static void pad_to_matrix(int pad)
{
    volatile uint32_t *mux = reg(ESP32S3_IO_MUX_REG((uint32_t)pad));
    *mux = (*mux & ~ESP32S3_IO_MUX_MCU_SEL_M) | (ESP32S3_IO_MUX_FUNC_GPIO << ESP32S3_IO_MUX_MCU_SEL_S) | ESP32S3_IO_MUX_IE;
}

static void route_output(int pad, uint32_t signal)
{
    pad_to_matrix(pad);
    *reg(ESP32S3_GPIO_FUNC_OUT_SEL_REG((uint32_t)pad)) = signal;                /* the output enable is the signal's own */
}

static void route_input(int pad, uint32_t signal)
{
    pad_to_matrix(pad);
    *reg(ESP32S3_GPIO_FUNC_IN_SEL_REG(signal)) = ESP32S3_GPIO_IN_SEL_ROUTED | (uint32_t)pad;
}

static void unroute_output(int pad)
{
    if (pad >= 0)
        *reg(ESP32S3_GPIO_FUNC_OUT_SEL_REG((uint32_t)pad)) = 256UL;             /* the GPIO output register again */
}

static void periph_clock(unsigned index, bool enable)
{
    Dmod_EnterCritical();
    if (enable)
        *reg(ESP32S3_PERIP_CLK_EN0_REG) |= ESP32S3_SPI_PERIP_BIT(index);
    else
        *reg(ESP32S3_PERIP_CLK_EN0_REG) &= ~ESP32S3_SPI_PERIP_BIT(index);
    Dmod_ExitCritical();
}

static void periph_reset(unsigned index)
{
    Dmod_EnterCritical();
    *reg(ESP32S3_PERIP_RST_EN0_REG) |= ESP32S3_SPI_PERIP_BIT(index);
    *reg(ESP32S3_PERIP_RST_EN0_REG) &= ~ESP32S3_SPI_PERIP_BIT(index);
    Dmod_ExitCritical();
}

static void apply(unsigned index)
{
    uint32_t guard = 100000U;
    *creg(index, SPI_CMD_REG) |= SPI_CMD_UPDATE;
    while ((*creg(index, SPI_CMD_REG) & SPI_CMD_UPDATE) && --guard != 0U) { }
}

/* The divider of the 80 MHz clock for `hz` (the same search as the HAL of ESP-IDF: pre 1..16, n 2..64, a 50 % duty) */
static uint32_t clock_register(uint32_t hz, uint32_t *effective)
{
    const uint32_t fapb = ESP32S3_SPI_CLOCK_HZ;
    if (hz > (fapb / 4U) * 3U)
    {
        *effective = fapb;
        return SPI_CLOCK_CLK_EQU_SYSCLK;
    }
    int best_n = -1, best_pre = -1;
    uint32_t best_err = 0;
    for (int n = 2; n <= 64; n++)
    {
        int pre = (int)(((fapb / (uint32_t)n) + hz / 2U) / hz);
        if (pre <= 0)
            pre = 1;
        if (pre > 16)
            pre = 16;
        uint32_t freq = fapb / (uint32_t)pre / (uint32_t)n;
        uint32_t err = freq > hz ? freq - hz : hz - freq;
        if (best_n == -1 || err <= best_err)
        {
            best_err = err;
            best_n = n;
            best_pre = pre;
        }
    }
    uint32_t h = (128U * (uint32_t)best_n + 127U) / 256U;
    if (h == 0U)
        h = 1U;
    *effective = fapb / (uint32_t)best_pre / (uint32_t)best_n;
    return ((uint32_t)(best_n - 1) << 12) | ((uint32_t)(best_pre - 1) << 18) | ((h - 1U) << 6) | (uint32_t)(best_n - 1);
}

static void configure_mode(unsigned index)
{
    const spi_t *s = &controllers[index];
    bool idle_high = s->polarity == dmspi_clock_polarity_high;
    bool second = s->phase == dmspi_clock_phase_2_edge;
    /* modes 0..3: (idle, out edge) = (0, 0) (0, 1) (1, 1) (1, 0) */
    bool out_edge = idle_high ? !second : second;

    *creg(index, SPI_MISC_REG) = (*creg(index, SPI_MISC_REG) & ~SPI_MISC_CK_IDLE_EDGE) | (idle_high ? SPI_MISC_CK_IDLE_EDGE : 0U);
    *creg(index, SPI_USER_REG) = (*creg(index, SPI_USER_REG) & ~SPI_USER_CK_OUT_EDGE) | (out_edge ? SPI_USER_CK_OUT_EDGE : 0U);
    uint32_t order = s->bit_order == dmspi_bit_order_lsb_first ? (SPI_CTRL_RD_BIT_ORDER | SPI_CTRL_WR_BIT_ORDER) : 0U;
    *creg(index, SPI_CTRL_REG) = (*creg(index, SPI_CTRL_REG) & ~(SPI_CTRL_RD_BIT_ORDER | SPI_CTRL_WR_BIT_ORDER)) | order;
    if (s->nss_mode == dmspi_nss_mode_hard)
    {
        *creg(index, SPI_MISC_REG) &= ~SPI_MISC_CS0_DIS;
        *creg(index, SPI_USER_REG) |= SPI_USER_CS_SETUP | SPI_USER_CS_HOLD;
    }
    else
    {
        *creg(index, SPI_MISC_REG) |= SPI_MISC_CS0_DIS;
        *creg(index, SPI_USER_REG) &= ~(SPI_USER_CS_SETUP | SPI_USER_CS_HOLD);
    }
    apply(index);
}

dmod_dmspi_port_api_declaration(1.0, int, _set_pins, (dmspi_instance_t instance, int sck_pin, int mosi_pin, int miso_pin, int cs_pin))
{
    int index = index_of(instance);
    if (index < 0 || !pad_valid(sck_pin) || !pad_valid(mosi_pin) || !pad_valid(miso_pin) || !pad_valid(cs_pin))
        return -EINVAL;
    spi_t *s = &controllers[index];
    if (s->initialised)
        return -EBUSY;
    if (sck_pin >= 0)
        s->sck = sck_pin;
    if (mosi_pin >= 0)
        s->mosi = mosi_pin;
    if (miso_pin >= 0)
        s->miso = miso_pin;
    if (cs_pin >= 0)
        s->cs = cs_pin;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _init, (dmspi_instance_t instance, dmspi_role_t role))
{
    int index = index_of(instance);
    if (index < 0)
        return -1;
    spi_t *s = &controllers[index];
    if (role != dmspi_role_master || s->initialised || s->sck < 0 || (s->mosi < 0 && s->miso < 0))
        return -1;                                          /* no slave; the pads have to be given (sck_pin and mosi_pin / miso_pin) */
    if (*reg(ESP32S3_PERIP_CLK_EN0_REG) & ESP32S3_SPI_PERIP_BIT(index))
    {
        /* the controller is enabled at reset: only the reset below puts it in a known state */
    }

    s->initialised = true;
    s->baudrate = 0;
    s->polarity = dmspi_clock_polarity_low;
    s->phase = dmspi_clock_phase_1_edge;
    s->bit_order = dmspi_bit_order_msb_first;
    s->nss_mode = dmspi_nss_mode_soft;
    s->trigger = dmspi_int_trigger_off;

    periph_clock(index, true);
    periph_reset(index);
    *creg(index, SPI_CLK_GATE_REG) = SPI_CLK_GATE_CLK_EN | SPI_CLK_GATE_MST_CLK_ACTIVE | SPI_CLK_GATE_MST_CLK_SEL;
    *creg(index, SPI_SLAVE_REG) = 0;                        /* master */
    *creg(index, SPI_USER_REG) = SPI_USER_DOUTDIN | SPI_USER_USR_MOSI | SPI_USER_USR_MISO;
    *creg(index, SPI_USER1_REG) &= ~0x07FE0000UL;           /* no CS setup / hold time beyond the bits of USER */
    *creg(index, SPI_MISC_REG) = SPI_MISC_CS0_DIS;
    *creg(index, SPI_CTRL_REG) = 0;
    *creg(index, SPI_DMA_INT_CLR_REG) = SPI_DMA_INT_TRANS_DONE;

    if (s->sck >= 0)
        route_output(s->sck, ESP32S3_SPI_SIG_CLK(index));
    if (s->mosi >= 0)
        route_output(s->mosi, ESP32S3_SPI_SIG_MOSI(index));
    if (s->miso >= 0)
        route_input(s->miso, ESP32S3_SPI_SIG_MISO(index));
    if (s->cs >= 0)
        route_output(s->cs, ESP32S3_SPI_SIG_CS0(index));
    configure_mode(index);
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _deinit, (dmspi_instance_t instance))
{
    int index = index_of(instance);
    if (index < 0)
        return -1;
    spi_t *s = &controllers[index];
    if (!s->initialised)
        return 0;
    unroute_output(s->sck);
    unroute_output(s->mosi);
    unroute_output(s->cs);
    periph_reset(index);
    periph_clock(index, false);
    s->initialised = false;
    s->sck = s->mosi = s->miso = s->cs = -1;
    return 0;
}

/* One transaction of `size` <= 64 bytes; tx NULL: 0xFF is sent, rx NULL: what comes in is dropped */
static int transaction(unsigned index, const uint8_t *tx, uint8_t *rx, size_t size)
{
    uint32_t words = ((uint32_t)size + 3U) / 4U;
    for (uint32_t w = 0; w < words; w++)
    {
        uint32_t word = 0xFFFFFFFFUL;
        if (tx != NULL)
        {
            word = 0;
            for (uint32_t b = 0; b < 4U && w * 4U + b < size; b++)
                word |= (uint32_t)tx[w * 4U + b] << (8U * b);
            if ((w + 1U) * 4U > size)
                word |= 0xFFFFFFFFUL << (8U * (uint32_t)(size - w * 4U));
        }
        *creg(index, SPI_W0_REG + 4U * w) = word;
    }
    *creg(index, SPI_MS_DLEN_REG) = (uint32_t)size * 8U - 1U;
    *creg(index, SPI_DMA_INT_CLR_REG) = SPI_DMA_INT_TRANS_DONE;
    apply(index);
    *creg(index, SPI_CMD_REG) |= SPI_CMD_USR;

    /* 64 bytes at 1 kHz take 0.5 s: the deadline follows the bit rate */
    uint32_t hz = controllers[index].baudrate != 0U ? controllers[index].baudrate : 1000U;
    uint64_t deadline_ms = 20U + (uint64_t)size * 8U * 1000U / hz * 2U;
    uint64_t start = Dmod_GetUptime();
    while (!(*creg(index, SPI_DMA_INT_RAW_REG) & SPI_DMA_INT_TRANS_DONE))
    {
        if (Dmod_GetUptime() - start > deadline_ms)
            return -1;
    }
    *creg(index, SPI_DMA_INT_CLR_REG) = SPI_DMA_INT_TRANS_DONE;

    if (rx != NULL)
    {
        for (uint32_t w = 0; w < words; w++)
        {
            uint32_t word = *creg(index, SPI_W0_REG + 4U * w);
            for (uint32_t b = 0; b < 4U && w * 4U + b < size; b++)
                rx[w * 4U + b] = (uint8_t)(word >> (8U * b));
        }
    }
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _transfer, (dmspi_instance_t instance, const uint8_t *tx, uint8_t *rx, size_t size))
{
    int index = index_of(instance);
    if (index < 0 || !controllers[index].initialised || controllers[index].baudrate == 0U)
        return -1;
    for (size_t done = 0; done < size;)
    {
        size_t n = size - done < ESP32S3_SPI_BUFFER_BYTES ? size - done : ESP32S3_SPI_BUFFER_BYTES;
        if (transaction((unsigned)index, tx != NULL ? tx + done : NULL, rx != NULL ? rx + done : NULL, n) != 0)
            return -1;
        done += n;
    }
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _transmit, (dmspi_instance_t instance, const uint8_t *data, size_t size))
{
    if (data == NULL)
        return -1;
    return dmspi_port_transfer(instance, data, NULL, size);
}

dmod_dmspi_port_api_declaration(1.0, int, _receive, (dmspi_instance_t instance, uint8_t *data, size_t size, size_t *received))
{
    if (data == NULL || received == NULL)
        return -1;
    *received = 0;
    int rc = dmspi_port_transfer(instance, NULL, data, size);
    if (rc == 0)
        *received = size;
    return rc;
}

dmod_dmspi_port_api_declaration(1.0, bool, _is_busy, (dmspi_instance_t instance))
{
    int index = index_of(instance);
    return index >= 0 && controllers[index].initialised && (*creg((unsigned)index, SPI_CMD_REG) & SPI_CMD_USR) != 0U;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_role, (dmspi_instance_t instance, dmspi_role_t role))
{
    return index_of(instance) >= 0 && role == dmspi_role_master ? 0 : -1;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_role_t, _get_role, (dmspi_instance_t instance))
{
    (void)instance;
    return dmspi_role_master;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_baudrate, (dmspi_instance_t instance, dmspi_baudrate_t baudrate))
{
    int index = index_of(instance);
    if (index < 0 || !controllers[index].initialised || baudrate == 0U)
        return -1;
    uint32_t effective;
    *creg((unsigned)index, SPI_CLOCK_REG) = clock_register(baudrate, &effective);
    apply((unsigned)index);
    controllers[index].baudrate = effective;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_baudrate_t, _get_baudrate, (dmspi_instance_t instance))
{
    int index = index_of(instance);
    return index >= 0 ? controllers[index].baudrate : 0U;
}

#define SETTER(NAME, TYPE, FIELD)                                                                         \
    dmod_dmspi_port_api_declaration(1.0, int, _set_##NAME, (dmspi_instance_t instance, TYPE value))      \
    {                                                                                                     \
        int index = index_of(instance);                                                                   \
        if (index < 0 || !controllers[index].initialised)                                                 \
            return -1;                                                                                    \
        controllers[index].FIELD = value;                                                                 \
        configure_mode((unsigned)index);                                                                  \
        return 0;                                                                                         \
    }                                                                                                     \
    dmod_dmspi_port_api_declaration(1.0, TYPE, _get_##NAME, (dmspi_instance_t instance))                 \
    {                                                                                                     \
        int index = index_of(instance);                                                                   \
        return controllers[index < 0 ? 0 : index].FIELD;                                                  \
    }

SETTER(clock_polarity, dmspi_clock_polarity_t, polarity)
SETTER(clock_phase, dmspi_clock_phase_t, phase)
SETTER(bit_order, dmspi_bit_order_t, bit_order)
SETTER(nss_mode, dmspi_nss_mode_t, nss_mode)

dmod_dmspi_port_api_declaration(1.0, int, _set_interrupt_trigger, (dmspi_instance_t instance, dmspi_int_trigger_t trigger))
{
    int index = index_of(instance);
    if (index < 0 || trigger != dmspi_int_trigger_off)
        return -1;                                          /* no interrupts: the transfers are polled */
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _read_interrupt_trigger, (dmspi_instance_t instance, dmspi_int_trigger_t *out_trigger))
{
    if (index_of(instance) < 0 || out_trigger == NULL)
        return -1;
    *out_trigger = dmspi_int_trigger_off;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _add_interrupt_handler,
                                (dmspi_instance_t instance, dmspi_port_interrupt_handler_t handler, void *user_ptr))
{
    (void)user_ptr;
    return index_of(instance) >= 0 && handler != NULL ? 0 : -1;        /* never called */
}

dmod_dmspi_port_api_declaration(1.0, int, _remove_interrupt_handler, (dmspi_instance_t instance, void *user_ptr))
{
    (void)user_ptr;
    return index_of(instance) >= 0 ? 0 : -1;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_rx_ring, (dmspi_instance_t instance, dm_sw_ring_t ring))
{
    (void)ring;
    return index_of(instance) >= 0 ? 0 : -1;                           /* nothing fills a ring in the master role */
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    Dmod_Printf("dmspi port module initialized (esp32s3)\n");
    return 0;
}

int dmod_deinit(void)
{
    for (unsigned i = 0; i < ESP32S3_SPI_COUNT; i++)
        dmspi_port_deinit((dmspi_instance_t)(ESP32S3_SPI_FIRST_INSTANCE + i));
    Dmod_Printf("dmspi port module deinitialized (esp32s3)\n");
    return 0;
}
