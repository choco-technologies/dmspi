#ifndef DMSPI_LEASE_H
#define DMSPI_LEASE_H

#include "dmspi.h"

/**
 * @brief Handle to an already-configured dmspi instance, for other driver
 * modules (e.g. a sensor, flash, or display driver) that need to talk on
 * an SPI bus dmdevfs already created from a board config.
 *
 * This is a direct Built-in API dependency (dmod_link_modules(<you> dmspi)),
 * resolved by the loader at module-load time - unlike opening the bus
 * through its /dev path, it does not depend on dmdevfs's /dev mount being
 * "ready", so it is safe to call dmspi_acquire() from your own driver's
 * _create(), during the same boot-time config scan dmspi itself runs in
 * (give your board-config section a driver_order after dmspi's, so the
 * instance already exists by the time you acquire it).
 */
typedef struct dmspi_lease *dmspi_lease_t;

/**
 * @brief Acquire a handle to dmspi instance number 'instance'.
 *
 * @return 0 on success, -EINVAL for an out-of-range instance number,
 *         -ENODEV if no dmspi instance with that number has been created
 *         (yet, or at all - check driver_order in your board config).
 */
dmod_dmspi_api(1.0, int, _acquire, (dmspi_instance_t instance, dmspi_lease_t *out));

/**
 * @brief Release a handle acquired with dmspi_acquire().
 *
 * dmspi instances live for the whole boot lifetime in practice (nothing
 * in this codebase tears one down while another driver still holds a
 * reference to it), so this does not free anything today - it exists so
 * every acquire has a matching release, which is what makes it safe to
 * add real lifetime tracking later without having to touch every caller.
 */
dmod_dmspi_api(1.0, void, _release, (dmspi_lease_t lease));

/**
 * @brief Full-duplex transfer on a leased bus.
 *
 * Equivalent to opening the instance's /dev path and issuing
 * dmspi_ioctl_cmd_transfer - same CS handling (asserts/deasserts cs_pin
 * around the transfer, in role=master), same 8-bit frames. Safe to call
 * from multiple driver modules sharing one leased instance (each call is
 * serialized against every other _transfer/_write/_read on this instance,
 * however it was reached - leased or through /dev - by an internal mutex),
 * but each caller is still responsible for its own CS pin if the bus has
 * more than one slave device - see docs/configuration.md's "Chip select"
 * section.
 *
 * @param tx   Bytes to send, or NULL to send 0xFF for every byte
 *             (dmspi_port_transfer's own convention).
 * @param rx   Buffer to receive into, or NULL to discard received bytes.
 * @param size Number of bytes to transfer.
 * @return 0 on success, a negative errno-style code on failure.
 */
dmod_dmspi_api(1.0, int, _transfer, (dmspi_lease_t lease, const uint8_t *tx, uint8_t *rx, size_t size));

#endif // DMSPI_LEASE_H
