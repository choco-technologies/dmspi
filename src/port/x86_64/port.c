/**
 * @file port.c
 * @brief dmspi_port implementation for the x86_64 host.
 *
 * There is no real SPI peripheral to drive on a regular CI runner, so this
 * port loops every instance back on itself in software: bytes handed to
 * _transmit/_transfer's tx side are queued into that instance's own FIFO,
 * and _receive/_transfer's rx side drain the same FIFO - exactly what a
 * master would see if its MOSI pin were wired straight to its MISO pin.
 * This exists purely so dmspi's dmod_loader-based tests (see ../../tests)
 * can enable the full dmspi module - with its declared dmspi_port
 * dependency - on a host that will never see real hardware.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmspi_port.h"
#include "dmod.h"
#include <string.h>

#define DMSPI_PORT_MAX_INSTANCES 4
#define DMSPI_PORT_FIFO_SIZE     256

typedef struct
{
    bool                            in_use;
    dmspi_role_t                    role;
    dmspi_baudrate_t                baudrate;
    dmspi_clock_polarity_t          clock_polarity;
    dmspi_clock_phase_t             clock_phase;
    dmspi_bit_order_t               bit_order;
    dmspi_nss_mode_t                nss_mode;
    dmspi_int_trigger_t             interrupt_trigger;
    dmspi_port_interrupt_handler_t  irq_handler;
    void*                           irq_user_ptr;
    dm_sw_ring_t                    rx_ring;

    uint8_t fifo[DMSPI_PORT_FIFO_SIZE];
    size_t  fifo_head;
    size_t  fifo_count;
} instance_state_t;

static instance_state_t g_instances[DMSPI_PORT_MAX_INSTANCES];

static int validate_instance(dmspi_instance_t instance)
{
    return (instance >= 1 && instance <= DMSPI_PORT_MAX_INSTANCES) ? 0 : -1;
}

static instance_state_t* get_instance(dmspi_instance_t instance)
{
    return &g_instances[instance - 1];
}

/* Queues one byte for loopback and fans it out to whichever consumer is
 * listening: a registered RX ring (drained by _receive), an interrupt
 * handler (if rx_not_empty is armed), or this instance's own FIFO (drained
 * directly by _receive/_transfer when no ring is registered). */
static void loopback_byte(dmspi_instance_t instance, instance_state_t* s, uint8_t byte)
{
    if (s->rx_ring != NULL)
    {
        dm_sw_ring_write(s->rx_ring, &byte, 1);
    }
    else
    {
        if (s->fifo_count >= DMSPI_PORT_FIFO_SIZE)
        {
            /* Overrun: drop the oldest queued byte rather than block, same
             * as a real peripheral's OVR would lose data instead of stall. */
            s->fifo_head = (s->fifo_head + 1) % DMSPI_PORT_FIFO_SIZE;
            s->fifo_count--;
        }
        s->fifo[(s->fifo_head + s->fifo_count) % DMSPI_PORT_FIFO_SIZE] = byte;
        s->fifo_count++;
    }

    if (s->irq_handler != NULL && (s->interrupt_trigger & dmspi_int_trigger_rx_not_empty) != 0)
    {
        s->irq_handler(s->irq_user_ptr, instance, dmspi_int_trigger_rx_not_empty, byte);
    }
}

static size_t drain_fifo(instance_state_t* s, uint8_t* data, size_t size)
{
    size_t n = (s->fifo_count < size) ? s->fifo_count : size;
    for (size_t i = 0; i < n; i++)
    {
        data[i] = s->fifo[(s->fifo_head + i) % DMSPI_PORT_FIFO_SIZE];
    }
    s->fifo_head = (s->fifo_head + n) % DMSPI_PORT_FIFO_SIZE;
    s->fifo_count -= n;
    return n;
}

int dmod_init(const Dmod_Config_t *Config)
{
    memset(g_instances, 0, sizeof(g_instances));
    Dmod_Printf("dmspi port module initialized (x86_64 loopback stub)\n");
    return 0;
}

int dmod_deinit(void)
{
    Dmod_Printf("dmspi port module deinitialized (x86_64 loopback stub)\n");
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _init, ( dmspi_instance_t instance, dmspi_role_t role ))
{
    if (validate_instance(instance) != 0) return -1;

    instance_state_t* s = get_instance(instance);
    memset(s, 0, sizeof(*s));
    s->in_use    = true;
    s->role      = role;
    s->bit_order = dmspi_bit_order_msb_first;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _deinit, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return -1;

    instance_state_t* s = get_instance(instance);
    memset(s, 0, sizeof(*s));
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _transfer, ( dmspi_instance_t instance, const uint8_t* tx, uint8_t* rx, size_t size ))
{
    if (validate_instance(instance) != 0) return -1;

    instance_state_t* s = get_instance(instance);
    for (size_t i = 0; i < size; i++)
    {
        uint8_t tx_byte = tx ? tx[i] : 0xFFU;
        loopback_byte(instance, s, tx_byte);

        uint8_t rx_byte = 0xFFU;
        drain_fifo(s, &rx_byte, 1);
        if (rx != NULL) rx[i] = rx_byte;
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

    instance_state_t* s = get_instance(instance);
    if (s->rx_ring != NULL)
    {
        *received = (size_t)dm_sw_ring_read(s->rx_ring, data, (dm_sw_ring_capacity_t)size);
    }
    else
    {
        *received = drain_fifo(s, data, size);
    }
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, bool, _is_busy, ( dmspi_instance_t instance ))
{
    return false;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_role, ( dmspi_instance_t instance, dmspi_role_t role ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->role = role;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_role_t, _get_role, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_role_master;
    return get_instance(instance)->role;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_baudrate, ( dmspi_instance_t instance, dmspi_baudrate_t baudrate ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->baudrate = baudrate;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_baudrate_t, _get_baudrate, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return 0;
    return get_instance(instance)->baudrate;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_clock_polarity, ( dmspi_instance_t instance, dmspi_clock_polarity_t polarity ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->clock_polarity = polarity;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_clock_polarity_t, _get_clock_polarity, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_clock_polarity_low;
    return get_instance(instance)->clock_polarity;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_clock_phase, ( dmspi_instance_t instance, dmspi_clock_phase_t phase ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->clock_phase = phase;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_clock_phase_t, _get_clock_phase, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_clock_phase_1_edge;
    return get_instance(instance)->clock_phase;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_bit_order, ( dmspi_instance_t instance, dmspi_bit_order_t bit_order ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->bit_order = bit_order;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_bit_order_t, _get_bit_order, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_bit_order_msb_first;
    return get_instance(instance)->bit_order;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_nss_mode, ( dmspi_instance_t instance, dmspi_nss_mode_t nss_mode ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->nss_mode = nss_mode;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, dmspi_nss_mode_t, _get_nss_mode, ( dmspi_instance_t instance ))
{
    if (validate_instance(instance) != 0) return dmspi_nss_mode_soft;
    return get_instance(instance)->nss_mode;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_interrupt_trigger, ( dmspi_instance_t instance, dmspi_int_trigger_t trigger ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->interrupt_trigger = trigger;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _read_interrupt_trigger, ( dmspi_instance_t instance, dmspi_int_trigger_t *out_trigger ))
{
    if (validate_instance(instance) != 0 || out_trigger == NULL) return -1;
    *out_trigger = get_instance(instance)->interrupt_trigger;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _add_interrupt_handler,
    ( dmspi_instance_t instance, dmspi_port_interrupt_handler_t handler, void *user_ptr ))
{
    if (validate_instance(instance) != 0 || handler == NULL) return -1;

    instance_state_t* s = get_instance(instance);
    s->irq_handler  = handler;
    s->irq_user_ptr = user_ptr;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _remove_interrupt_handler,
    ( dmspi_instance_t instance, void *user_ptr ))
{
    if (validate_instance(instance) != 0) return -1;

    instance_state_t* s = get_instance(instance);
    if (s->irq_user_ptr != user_ptr) return -1;

    s->irq_handler  = NULL;
    s->irq_user_ptr = NULL;
    return 0;
}

dmod_dmspi_port_api_declaration(1.0, int, _set_rx_ring,
    ( dmspi_instance_t instance, dm_sw_ring_t ring ))
{
    if (validate_instance(instance) != 0) return -1;
    get_instance(instance)->rx_ring = ring;
    return 0;
}
