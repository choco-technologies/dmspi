#ifndef DMSPI_TYPES_H
#define DMSPI_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief SPI instance type (1-based, e.g. 1 = SPI1)
 */
typedef uint8_t dmspi_instance_t;

/**
 * @brief SPI requested clock frequency, in Hz (master mode only)
 */
typedef uint32_t dmspi_baudrate_t;

/**
 * @brief SPI role - master drives SCK/NSS, slave is clocked externally
 */
typedef enum
{
    dmspi_role_master = 0,      /**< This instance drives the bus */
    dmspi_role_slave,           /**< This instance is clocked by an external master */
} dmspi_role_t;

/**
 * @brief SPI clock polarity (CPOL)
 */
typedef enum
{
    dmspi_clock_polarity_low = 0,   /**< Clock idles low */
    dmspi_clock_polarity_high,      /**< Clock idles high */
} dmspi_clock_polarity_t;

/**
 * @brief SPI clock phase (CPHA)
 */
typedef enum
{
    dmspi_clock_phase_1_edge = 0,   /**< Data sampled on the first clock edge */
    dmspi_clock_phase_2_edge,       /**< Data sampled on the second clock edge */
} dmspi_clock_phase_t;

/**
 * @brief SPI bit order
 */
typedef enum
{
    dmspi_bit_order_msb_first = 0,  /**< MSB transmitted first (default) */
    dmspi_bit_order_lsb_first,      /**< LSB transmitted first */
} dmspi_bit_order_t;

/**
 * @brief NSS (chip select) management mode
 */
typedef enum
{
    dmspi_nss_mode_soft = 0,    /**< Software managed - master: CS handled externally
                                  *   (e.g. via dmgpio); slave: instance always selected */
    dmspi_nss_mode_hard,        /**< Hardware managed - master: NSS pin driven by the
                                  *   peripheral (single-slave only); slave: physical
                                  *   NSS pin gates the peripheral */
} dmspi_nss_mode_t;

/**
 * @brief SPI interrupt trigger source
 */
typedef enum
{
    dmspi_int_trigger_off        = 0,        /**< Interrupts disabled */
    dmspi_int_trigger_rx_not_empty = (1 << 0), /**< RX buffer not empty */
    dmspi_int_trigger_tx_empty   = (1 << 1), /**< TX buffer empty */
    dmspi_int_trigger_error      = (1 << 2), /**< Mode fault / overrun / CRC error */
} dmspi_int_trigger_t;

/**
 * @brief IOCTL commands for DMSPI device
 */
typedef enum
{
    dmspi_ioctl_cmd_get_role = 1,               /**< Get role (master/slave) */
    dmspi_ioctl_cmd_set_role,                   /**< Set role (master/slave) */
    dmspi_ioctl_cmd_get_baudrate,               /**< Get requested baud rate */
    dmspi_ioctl_cmd_set_baudrate,               /**< Set requested baud rate */
    dmspi_ioctl_cmd_get_clock_polarity,         /**< Get clock polarity (CPOL) */
    dmspi_ioctl_cmd_set_clock_polarity,         /**< Set clock polarity (CPOL) */
    dmspi_ioctl_cmd_get_clock_phase,            /**< Get clock phase (CPHA) */
    dmspi_ioctl_cmd_set_clock_phase,            /**< Set clock phase (CPHA) */
    dmspi_ioctl_cmd_get_bit_order,              /**< Get bit order */
    dmspi_ioctl_cmd_set_bit_order,              /**< Set bit order */
    dmspi_ioctl_cmd_get_nss_mode,               /**< Get NSS management mode */
    dmspi_ioctl_cmd_set_nss_mode,               /**< Set NSS management mode */
    dmspi_ioctl_cmd_set_interrupt_handler,      /**< Set interrupt handler; arg = dmspi_interrupt_handler_t* */
    dmspi_ioctl_cmd_reconfigure,                /**< Reconfigure SPI with current settings */
    dmspi_ioctl_cmd_transfer,                   /**< Full-duplex transfer; arg = dmspi_transfer_t* */

    dmspi_ioctl_cmd_max
} dmspi_ioctl_cmd_t;

/**
 * @brief Opaque driver context type (forward declaration)
 *
 * The concrete definition is private to the dmspi driver.
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

/**
 * @brief Full-duplex transfer descriptor, used with dmspi_ioctl_cmd_transfer.
 *
 * Either @p tx or @p rx may be NULL: a NULL @p tx shifts out a dummy byte
 * (0xFF) for every position, a NULL @p rx discards received bytes.
 */
typedef struct
{
    const uint8_t *tx;   /**< Bytes to transmit (may be NULL) */
    uint8_t       *rx;   /**< Buffer to receive into (may be NULL) */
    size_t         size; /**< Number of bytes to exchange */
} dmspi_transfer_t;

/**
 * @brief Parameters passed to a dmhaman-registered interrupt handler.
 */
typedef struct
{
    dmspi_instance_t     instance;    /**< SPI instance that generated the interrupt */
    dmspi_int_trigger_t  trigger;     /**< Which trigger fired */
    uint8_t              data;        /**< Received data byte (valid for rx_not_empty trigger) */
} dmspi_interrupt_params_t;

/**
 * @brief SPI interrupt handler function type
 *
 * @param context   Context of the driver instance
 * @param instance  SPI instance that generated the interrupt
 * @param trigger   Which interrupt trigger fired
 * @param data      Received data byte (valid when trigger includes rx_not_empty)
 */
typedef void (*dmspi_interrupt_handler_t)(dmdrvi_context_t context, dmspi_instance_t instance, dmspi_int_trigger_t trigger, uint8_t data);

/**
 * @brief SPI port interrupt handler function type
 *
 * Called by the port layer when an interrupt occurs on a SPI instance.
 *
 * @param user_ptr  User pointer supplied at registration time (e.g. driver context)
 * @param instance  SPI instance that generated the interrupt
 * @param trigger   Which interrupt trigger fired
 * @param data      Received data byte (valid when trigger includes rx_not_empty)
 */
typedef void (*dmspi_port_interrupt_handler_t)(void *user_ptr, dmspi_instance_t instance, dmspi_int_trigger_t trigger, uint8_t data);

#endif /* DMSPI_TYPES_H */
