#ifndef DMSPI_PORT_H
#define DMSPI_PORT_H

#include "dmod_types.h"
#include "dmspi_port_defs.h"
#include "dmspi_types.h"
#include "dm_sw_ring.h"

/* --- Lifecycle --- */

dmod_dmspi_port_api(1.0, int, _init,   ( dmspi_instance_t instance, dmspi_role_t role ) );
dmod_dmspi_port_api(1.0, int, _deinit, ( dmspi_instance_t instance ) );

/* --- Data transfer ---
 *
 * All transfers are full-duplex at the hardware level: _transmit discards
 * the bytes shifted in while sending, _receive shifts out a dummy byte
 * (0xFF) for every byte it reads, and _transfer exposes both directions at
 * once. In slave mode these calls block waiting for the external master to
 * clock each byte - configure a RX ring (see _set_rx_ring) and an interrupt
 * trigger for non-blocking slave reception instead.
 */

dmod_dmspi_port_api(1.0, int, _transmit, ( dmspi_instance_t instance, const uint8_t* data, size_t size ) );
dmod_dmspi_port_api(1.0, int, _receive,  ( dmspi_instance_t instance, uint8_t* data, size_t size, size_t* received ) );
dmod_dmspi_port_api(1.0, int, _transfer, ( dmspi_instance_t instance, const uint8_t* tx, uint8_t* rx, size_t size ) );

/* --- Status --- */

dmod_dmspi_port_api(1.0, bool, _is_busy, ( dmspi_instance_t instance ) );

/* --- Role --- */

dmod_dmspi_port_api(1.0, int,         _set_role, ( dmspi_instance_t instance, dmspi_role_t role ) );
dmod_dmspi_port_api(1.0, dmspi_role_t, _get_role, ( dmspi_instance_t instance ) );

/* --- Baud rate (master only) --- */

dmod_dmspi_port_api(1.0, int,              _set_baudrate, ( dmspi_instance_t instance, dmspi_baudrate_t baudrate ) );
dmod_dmspi_port_api(1.0, dmspi_baudrate_t, _get_baudrate, ( dmspi_instance_t instance ) );

/* --- Clock polarity / phase --- */

dmod_dmspi_port_api(1.0, int,                    _set_clock_polarity, ( dmspi_instance_t instance, dmspi_clock_polarity_t polarity ) );
dmod_dmspi_port_api(1.0, dmspi_clock_polarity_t, _get_clock_polarity, ( dmspi_instance_t instance ) );
dmod_dmspi_port_api(1.0, int,                    _set_clock_phase,    ( dmspi_instance_t instance, dmspi_clock_phase_t phase ) );
dmod_dmspi_port_api(1.0, dmspi_clock_phase_t,    _get_clock_phase,    ( dmspi_instance_t instance ) );

/* --- Bit order --- */

dmod_dmspi_port_api(1.0, int,               _set_bit_order, ( dmspi_instance_t instance, dmspi_bit_order_t bit_order ) );
dmod_dmspi_port_api(1.0, dmspi_bit_order_t, _get_bit_order, ( dmspi_instance_t instance ) );

/* --- NSS management --- */

dmod_dmspi_port_api(1.0, int,              _set_nss_mode, ( dmspi_instance_t instance, dmspi_nss_mode_t nss_mode ) );
dmod_dmspi_port_api(1.0, dmspi_nss_mode_t, _get_nss_mode, ( dmspi_instance_t instance ) );

/* --- Interrupt trigger --- */

dmod_dmspi_port_api(1.0, int, _set_interrupt_trigger,  ( dmspi_instance_t instance, dmspi_int_trigger_t trigger ) );
dmod_dmspi_port_api(1.0, int, _read_interrupt_trigger, ( dmspi_instance_t instance, dmspi_int_trigger_t *out_trigger ) );

/* --- Interrupt handler registration --- */

dmod_dmspi_port_api(1.0, int, _add_interrupt_handler,
    ( dmspi_instance_t instance, dmspi_port_interrupt_handler_t handler, void *user_ptr ) );
dmod_dmspi_port_api(1.0, int, _remove_interrupt_handler,
    ( dmspi_instance_t instance, void *user_ptr ) );

/* --- SW ring buffer (RX only - see _add_interrupt_handler for TX events) --- */

dmod_dmspi_port_api(1.0, int, _set_rx_ring,
    ( dmspi_instance_t instance, dm_sw_ring_t ring ) );

#endif // DMSPI_PORT_H
