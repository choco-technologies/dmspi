#ifndef DMSPI_H
#define DMSPI_H

#include "dmspi_defs.h"
#include "dmspi_types.h"

/**
 * @brief SPI driver configuration structure
 */
typedef struct
{
    dmspi_instance_t        instance;           /**< SPI instance number (1-based) */
    dmspi_role_t             role;                /**< Master or slave */
    dmspi_baudrate_t         baudrate;            /**< Requested SCK frequency, Hz (master only) */
    dmspi_clock_polarity_t   clock_polarity;      /**< Clock polarity (CPOL) */
    dmspi_clock_phase_t      clock_phase;         /**< Clock phase (CPHA) */
    dmspi_bit_order_t        bit_order;           /**< Bit order (MSB/LSB first) */
    dmspi_nss_mode_t         nss_mode;            /**< NSS (chip select) management mode */
    dmspi_int_trigger_t      interrupt_trigger;   /**< Interrupt trigger source */
    dmspi_interrupt_handler_t interrupt_handler;  /**< Interrupt handler (NULL = not used) */
} dmspi_config_t;

/**
 * @brief Validate a configuration structure without touching hardware.
 *
 * Checks that the combination of parameters is self-consistent (e.g. a
 * master requires a non-zero baud rate). Does not require the dmspi_port
 * module to be loaded, so it is safe to call from any context, including
 * off-target unit tests.
 *
 * @return true if the configuration is valid, false otherwise.
 */
dmod_dmspi_api(1.0, bool, _validate_config, ( const dmspi_config_t *config ));

#endif // DMSPI_H
