#ifndef DMSPI_PORT_H
#define DMSPI_PORT_H

#include "dmod_types.h"
#include "dmspi_port_defs.h"

/*
 * Declare architecture-independent port API functions here, following the
 * dmod_dmspi_port_api(version, return_type, _suffix, (args)) pattern,
 * e.g.:
 *
 *   dmod_dmspi_port_api(1.0, int, _configure, ( int some_arg ) );
 *
 * Each declaration here must have a matching *definition* in
 * src/port/<arch>/port.c (or a shared src/port/<arch>_common/ file), written
 * with the dmod_dmspi_port_api_declaration(...) macro instead:
 *
 *   dmod_dmspi_port_api_declaration(1.0, int, _configure, ( int some_arg ) )
 *   {
 *       ...
 *   }
 *
 * Note this is a *different* mechanism from the dmod_init()/dmod_deinit()
 * module lifecycle hooks already defined in port.c - don't declare `_init`/
 * `_deinit` here too, that name collides with the lifecycle hooks and (unlike
 * them) requires an explicit dmod_dmspi_port_api_declaration(...)
 * definition to avoid an undefined-reference link error.
 *
 * See dmfmc/include/dmfmc_port.h and dmfmc/src/port/stm32_common/stm32_common.c
 * for a fully worked example.
 */

#endif // DMSPI_PORT_H
