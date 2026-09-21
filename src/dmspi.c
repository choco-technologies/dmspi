#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmspi.h"
#include "dmspi_port.h"
#include "dmdrvi.h"
#include "dmhaman.h"
#include "dmini.h"
#include "dm_sw_ring.h"
#include "dmgpio_lease.h"
#include <errno.h>
#include <string.h>

/* Magic set to DSPI */
#define DMSPI_CONTEXT_MAGIC    0x44535049

/* ---- Chip-select (CS/NSS) management ----
 *
 * nss_mode=soft leaves NSS entirely unmanaged by the SPI peripheral - by
 * design, since a single soft-NSS bus can have many slaves, each with its
 * own CS line, and only the caller knows which one applies to a given
 * transaction. For the common case of ONE fixed slave device, this driver
 * can still drive that CS line automatically: cs_pin names a GPIO pin
 * (e.g. "PE3" - same syntax as pin= in a dmgpio board-config section) and
 * this driver asserts it around every _write/_read/transfer-ioctl call in
 * master mode, deasserting it again once the transfer completes. Ignored
 * for role=slave - a slave never drives its own CS.
 *
 * The pin is claimed through dmgpio's lease API (dmgpio_lease.h,
 * dmod_dmgpio_api _pin_acquire/_release/_write) - a direct Built-in API
 * dependency (dmod_link_modules(dmspi dmgpio), see CMakeLists.txt),
 * resolved by the loader at module-load time. That replaces an earlier
 * cs_path design (a dmdevfs /dev path, opened lazily via Dmod_FileOpen/
 * Dmod_Ioctl against hardcoded dmgpio ioctl constants) and fixes what was
 * wrong with it:
 *   - no dmdevfs path format to get right (port-index-in-path etc) -
 *     cs_pin reuses the exact "PA5" syntax every other pin= in these
 *     config files already uses;
 *   - no hardcoded copy of dmgpio's ioctl ABI - the lease functions are
 *     real, typed, linked declarations;
 *   - no boot-ordering hazard - dmgpio_pin_acquire() is a declared module
 *     dependency, not a runtime /dev lookup, so it can run straight from
 *     _create() and a failure (e.g. -EBUSY, the pin already leased to
 *     another driver) surfaces immediately at config time instead of
 *     silently on first transfer.
 * The lease also owns pin configuration itself (mode/speed/output type/
 * initial level), so cs_pin replaces the separate dmgpio [xxx_cs] board-
 * config section entirely - see configs/README.md.
 */

/**
 * @brief DMDRVI context structure
 */
struct dmdrvi_context
{
    uint32_t            magic;                  /**< Magic number for validation */
    dmspi_config_t       config;                 /**< Configuration parameters */
    char                *interrupt_handler_name; /**< dmhaman handler name (NULL = not used) */
    dm_sw_ring_t         rx_ring;                /**< Software ring buffer for received bytes */
    uint32_t             rx_ring_size;           /**< Capacity of the RX ring buffer (from config) */
    dm_sw_ring_flags_t   rx_ring_wait_flags;     /**< RX ring read-wait behavior (from config) */
    dmgpio_lease_t       cs_lease;               /**< CS pin lease (NULL = unmanaged) */
    bool                 cs_active_high;         /**< true = assert by driving the pin high instead of low */
};

static void cs_assert(dmdrvi_context_t context)
{
    if (context->cs_lease != NULL)
        dmgpio_pin_write(context->cs_lease, context->cs_active_high);
}

static void cs_deassert(dmdrvi_context_t context)
{
    if (context->cs_lease != NULL)
        dmgpio_pin_write(context->cs_lease, !context->cs_active_high);
}

static int is_valid_context(dmdrvi_context_t context)
{
    return (context != NULL && context->magic == DMSPI_CONTEXT_MAGIC);
}

/* ---- Interrupt dispatch ---- */

/* Dispatches port interrupt events to a dmhaman-registered handler. */
static void internal_interrupt_handler(void *user_ptr, dmspi_instance_t instance,
                                        dmspi_int_trigger_t trigger, uint8_t data)
{
    dmdrvi_context_t ctx = (dmdrvi_context_t)user_ptr;

    if (ctx->interrupt_handler_name != NULL)
    {
        dmspi_interrupt_params_t params;
        params.instance = instance;
        params.trigger  = trigger;
        params.data     = data;
        dmhaman_call_handler(ctx->interrupt_handler_name, &params);
    }
}

/* ---- String conversion helpers ---- */

static int string_to_role(const char *s, dmspi_role_t *out_role)
{
    if (s != NULL)
    {
        if (strcmp(s, "master") == 0) { *out_role = dmspi_role_master; return 0; }
        if (strcmp(s, "slave")  == 0) { *out_role = dmspi_role_slave;  return 0; }
    }
    return -1;
}

static dmspi_clock_polarity_t string_to_clock_polarity(const char *s)
{
    if (s != NULL && strcmp(s, "high") == 0) return dmspi_clock_polarity_high;
    return dmspi_clock_polarity_low;
}

static dmspi_clock_phase_t string_to_clock_phase(const char *s)
{
    if (s != NULL && strcmp(s, "2edge") == 0) return dmspi_clock_phase_2_edge;
    return dmspi_clock_phase_1_edge;
}

static dmspi_bit_order_t string_to_bit_order(const char *s)
{
    if (s != NULL && strcmp(s, "lsb_first") == 0) return dmspi_bit_order_lsb_first;
    return dmspi_bit_order_msb_first;
}

static dmspi_nss_mode_t string_to_nss_mode(const char *s)
{
    if (s != NULL && strcmp(s, "hard") == 0) return dmspi_nss_mode_hard;
    return dmspi_nss_mode_soft;
}

static dmspi_int_trigger_t string_to_interrupt_trigger(const char *s)
{
    if (s != NULL)
    {
        if (strcmp(s, "rx_not_empty") == 0) return dmspi_int_trigger_rx_not_empty;
        if (strcmp(s, "tx_empty")     == 0) return dmspi_int_trigger_tx_empty;
        if (strcmp(s, "error")        == 0) return dmspi_int_trigger_error;
    }
    return dmspi_int_trigger_off;
}

static dm_sw_ring_flags_t string_to_rx_ring_wait_flags(const char *s)
{
    if (s != NULL)
    {
        if (strcmp(s, "none")     == 0) return 0;
        if (strcmp(s, "all_data") == 0) return dm_sw_ring_flags_wait_for_all_data;
    }
    return dm_sw_ring_flags_wait_for_some_data;
}

/**
 * @brief Parse "P<port><index>" (e.g. "PE3") into dmgpio's pin encoding
 * (port*16 + index, see dmgpio_lease.h) - the same syntax already used by
 * pin= in a dmgpio board-config section. Returns -1 on a malformed string.
 */
static int string_to_gpio_pin(const char *s)
{
    if (s == NULL || s[0] != 'P' || s[1] < 'A' || s[1] > 'K' || s[2] == '\0')
        return -1;

    int index = 0;
    for (const char *p = s + 2; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
            return -1;
        index = index * 10 + (*p - '0');
        if (index > 15)
            return -1;
    }

    return (int)(s[1] - 'A') * 16 + index;
}

/* ---- Configuration ---- */

/**
 * @brief Detect which INI section holds the SPI configuration.
 *
 * The driver supports two config file layouts:
 *   1. Standard: a [dmspi] section.
 *   2. Board / device-named: a section whose name is the device label
 *      (e.g. [flash_spi] in board config files).
 *
 * Detection strategy:
 *   a) If [dmspi] contains an 'instance' or 'role' key -> use "dmspi".
 *   b) Otherwise serialise the INI to a temporary string and scan for the
 *      first named section (skipping [main]) that contains 'instance' or
 *      'role'. Copy its name into section_buf and return it.
 *   c) Fall back to "dmspi" so the caller produces a meaningful error.
 */
static const char *detect_config_section(dmini_context_t ini,
                                          char *section_buf, size_t section_buf_sz)
{
    /* Fast path - standard [dmspi] section present */
    if (dmini_has_key(ini, "dmspi", "instance") || dmini_has_key(ini, "dmspi", "role"))
        return "dmspi";

    /* Slow path - serialise the INI and scan for the first usable section */
    int needed = dmini_generate_string(ini, NULL, 0);
    if (needed <= 1)
        return "dmspi";

    /* dmini_generate_string follows snprintf convention: the first call returns
     * the number of characters that would be written, NOT including the null
     * terminator. Allocate one extra byte so the null terminator always fits
     * and the scanning loop below is guaranteed to find it within the buffer. */
    char *ini_str = (char *)Dmod_Malloc((size_t)needed + 1);
    if (ini_str == NULL)
        return "dmspi";

    if (dmini_generate_string(ini, ini_str, (size_t)needed + 1) <= 0)
    {
        Dmod_Free(ini_str);
        return "dmspi";
    }
    ini_str[needed] = '\0';

    const char *result = "dmspi";
    char *p = ini_str;
    while (*p != '\0')
    {
        if (*p == '[')
        {
            char *name_start = p + 1;
            char *name_end   = name_start;
            while (*name_end != '\0' && *name_end != ']' &&
                   *name_end != '\n'  && *name_end != '\r')
                name_end++;

            if (*name_end == ']')
            {
                size_t name_len = (size_t)(name_end - name_start);
                if (name_len > 0 && name_len < section_buf_sz)
                {
                    memcpy(section_buf, name_start, name_len);
                    section_buf[name_len] = '\0';

                    if (strcmp(section_buf, "main") != 0 &&
                        (dmini_has_key(ini, section_buf, "instance") ||
                         dmini_has_key(ini, section_buf, "role")))
                    {
                        result = section_buf;
                        break;
                    }
                }
            }
        }
        p++;
    }

    Dmod_Free(ini_str);
    return result;
}

static int check_config_parameters(const dmspi_config_t *cfg)
{
    if (cfg->instance == 0)
    {
        DMOD_LOG_ERROR("Invalid SPI instance (must be >= 1)\n");
        return -EINVAL;
    }
    if (cfg->role == dmspi_role_master && cfg->baudrate == 0)
    {
        DMOD_LOG_ERROR("Baud rate not set in configuration (required for master role)\n");
        return -EINVAL;
    }
    return 0;
}

dmod_dmspi_api_declaration(1.0, bool, _validate_config, ( const dmspi_config_t *config ))
{
    return (config != NULL) && (check_config_parameters(config) == 0);
}

static int read_config_parameters(dmdrvi_context_t context, dmini_context_t config)
{
    char section_buf[64];
    const char *section = detect_config_section(config, section_buf, sizeof(section_buf));

    context->config.instance = (dmspi_instance_t)dmini_get_int(config, section, "instance", 1);

    if (string_to_role(dmini_get_string(config, section, "role", "master"), &context->config.role) != 0)
    {
        DMOD_LOG_ERROR("Invalid 'role' in [%s] config (expected master/slave)\n", section);
        return -EINVAL;
    }

    /* Optional "mode=0..3" shorthand (SPI mode 0-3), overridden by explicit
     * clock_polarity/clock_phase keys when both are present. */
    int spi_mode = dmini_get_int(config, section, "mode", -1);
    if (spi_mode >= 0 && spi_mode <= 3)
    {
        context->config.clock_polarity = (spi_mode & 0x2) ? dmspi_clock_polarity_high : dmspi_clock_polarity_low;
        context->config.clock_phase    = (spi_mode & 0x1) ? dmspi_clock_phase_2_edge  : dmspi_clock_phase_1_edge;
    }
    else
    {
        context->config.clock_polarity = dmspi_clock_polarity_low;
        context->config.clock_phase    = dmspi_clock_phase_1_edge;
    }
    if (dmini_has_key(config, section, "clock_polarity"))
        context->config.clock_polarity = string_to_clock_polarity(dmini_get_string(config, section, "clock_polarity", "low"));
    if (dmini_has_key(config, section, "clock_phase"))
        context->config.clock_phase = string_to_clock_phase(dmini_get_string(config, section, "clock_phase", "1edge"));

    context->config.baudrate    = (dmspi_baudrate_t)dmini_get_int(config, section, "baudrate", 0);
    context->config.bit_order   = string_to_bit_order(dmini_get_string(config, section, "bit_order", "msb_first"));
    context->config.nss_mode    = string_to_nss_mode(dmini_get_string(config, section, "nss_mode", "soft"));
    context->config.interrupt_trigger = string_to_interrupt_trigger(dmini_get_string(config, section, "interrupt_trigger", "off"));
    context->config.interrupt_handler = NULL;

    const char *handler_name = dmini_get_string(config, section, "interrupt_handler", NULL);
    context->interrupt_handler_name = (handler_name != NULL) ? Dmod_StrDup(handler_name) : NULL;

    context->rx_ring_size = (uint32_t)dmini_get_int(config, section, "rx_ring_size", 0);
    context->rx_ring_wait_flags = string_to_rx_ring_wait_flags(
        dmini_get_string(config, section, "rx_ring_wait_mode", "some_data"));

    return check_config_parameters(&context->config);
}

/**
 * @brief Claim and configure the CS pin named by 'cs_pin', if configured.
 *
 * Requires context->config.role to already be set (read_config_parameters()
 * must run first). A no-op (returns 0) when cs_pin is absent, or when
 * role=slave (a slave never drives its own CS - see the file header
 * comment on CS management). Fails outright - and so fails _create() - if
 * cs_pin is present but malformed, or if the pin can't be claimed (e.g.
 * already leased to another driver).
 */
static int setup_cs_pin(dmdrvi_context_t context, dmini_context_t config)
{
    char section_buf[64];
    const char *section = detect_config_section(config, section_buf, sizeof(section_buf));

    const char *cs_pin_str = dmini_get_string(config, section, "cs_pin", NULL);
    if (cs_pin_str == NULL)
        return 0;

    if (context->config.role != dmspi_role_master)
    {
        DMOD_LOG_ERROR("'cs_pin' is ignored for role=slave (a slave never drives its own CS)\n");
        return 0;
    }

    int pin = string_to_gpio_pin(cs_pin_str);
    if (pin < 0)
    {
        DMOD_LOG_ERROR("Invalid 'cs_pin' value '%s' (expected e.g. \"PE3\")\n", cs_pin_str);
        return -EINVAL;
    }

    context->cs_active_high = (strcmp(
        dmini_get_string(config, section, "cs_active_level", "low"), "high") == 0);

    /* Preload deasserted; mode/af are the only settings this plain output
     * pin needs, edge/user are unused (no interrupt). */
    int ret = dmgpio_pin_acquire((int16_t)pin, dmgpio_mode_output, 0,
                                  !context->cs_active_high, NULL, NULL, &context->cs_lease);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("Failed to claim CS pin '%s' (%d) - already in use?\n", cs_pin_str, ret);
        return ret;
    }

    return 0;
}

/**
 * @brief Apply configuration to the port layer
 */
static int configure(dmdrvi_context_t context)
{
    dmspi_config_t *c = &context->config;
    int ret;

    ret = dmspi_port_init(c->instance, c->role);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("Failed to initialize SPI%u\n", c->instance);
        return ret;
    }

    ret = dmspi_port_set_clock_polarity(c->instance, c->clock_polarity);
    if (ret != 0) goto err;

    ret = dmspi_port_set_clock_phase(c->instance, c->clock_phase);
    if (ret != 0) goto err;

    ret = dmspi_port_set_bit_order(c->instance, c->bit_order);
    if (ret != 0) goto err;

    ret = dmspi_port_set_nss_mode(c->instance, c->nss_mode);
    if (ret != 0) goto err;

    if (c->role == dmspi_role_master)
    {
        ret = dmspi_port_set_baudrate(c->instance, c->baudrate);
        if (ret != 0) goto err;
    }

    dmspi_int_trigger_t trigger = c->interrupt_trigger;
    if (context->rx_ring_size > 0)
        trigger |= dmspi_int_trigger_rx_not_empty;

    if (trigger != dmspi_int_trigger_off)
    {
        ret = dmspi_port_set_interrupt_trigger(c->instance, trigger);
        if (ret != 0) goto err;
    }

    DMOD_LOG_INFO("SPI%u configured: role=%s, %u Hz, CPOL=%u, CPHA=%u\n",
        c->instance, (c->role == dmspi_role_master) ? "master" : "slave",
        c->baudrate, (unsigned)c->clock_polarity, (unsigned)c->clock_phase);

    return 0;

err:
    DMOD_LOG_ERROR("Failed to configure SPI%u\n", c->instance);
    dmspi_port_deinit(c->instance);
    return ret;
}

/* ---- IOCTL helpers ---- */

static int update_configuration(dmspi_config_t *cfg, int command, void *arg)
{
    switch (command)
    {
        case dmspi_ioctl_cmd_set_role:
            cfg->role = *(dmspi_role_t *)arg;
            break;
        case dmspi_ioctl_cmd_set_baudrate:
            cfg->baudrate = *(dmspi_baudrate_t *)arg;
            break;
        case dmspi_ioctl_cmd_set_clock_polarity:
            cfg->clock_polarity = *(dmspi_clock_polarity_t *)arg;
            break;
        case dmspi_ioctl_cmd_set_clock_phase:
            cfg->clock_phase = *(dmspi_clock_phase_t *)arg;
            break;
        case dmspi_ioctl_cmd_set_bit_order:
            cfg->bit_order = *(dmspi_bit_order_t *)arg;
            break;
        case dmspi_ioctl_cmd_set_nss_mode:
            cfg->nss_mode = *(dmspi_nss_mode_t *)arg;
            break;
        default:
            return -EINVAL;
    }
    return check_config_parameters(cfg);
}

static int read_configuration(dmdrvi_context_t context, int command, void *arg)
{
    dmspi_config_t *c = &context->config;
    switch (command)
    {
        case dmspi_ioctl_cmd_get_role:
            *(dmspi_role_t *)arg = c->role;
            break;
        case dmspi_ioctl_cmd_get_baudrate:
            *(dmspi_baudrate_t *)arg = c->baudrate;
            break;
        case dmspi_ioctl_cmd_get_clock_polarity:
            *(dmspi_clock_polarity_t *)arg = c->clock_polarity;
            break;
        case dmspi_ioctl_cmd_get_clock_phase:
            *(dmspi_clock_phase_t *)arg = c->clock_phase;
            break;
        case dmspi_ioctl_cmd_get_bit_order:
            *(dmspi_bit_order_t *)arg = c->bit_order;
            break;
        case dmspi_ioctl_cmd_get_nss_mode:
            *(dmspi_nss_mode_t *)arg = c->nss_mode;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("DMSPI interface module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMSPI interface module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters to dmspi_dmdrvi_create\n");
        return NULL;
    }

    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;

    memset(context, 0, sizeof(*context));
    context->magic = DMSPI_CONTEXT_MAGIC;

    if (read_config_parameters(context, config) != 0 ||
        setup_cs_pin(context, config) != 0 ||
        configure(context) != 0)
    {
        DMOD_LOG_ERROR("Failed to create DMDRVI context with provided configuration\n");
        if (context->cs_lease != NULL)
            dmgpio_pin_release(context->cs_lease);
        Dmod_Free(context->interrupt_handler_name);
        Dmod_Free(context);
        return NULL;
    }

    /* Create RX ring buffer */
    if (context->rx_ring_size > 0)
    {
        /* No dm_sw_ring_flags_mutex_sync: this ring is written to directly from
         * the SPI RX interrupt handler, and taking a FreeRTOS mutex from ISR
         * context is invalid. The ring's lock-free critical-section path (used
         * when no mutex is configured) is safe for this single-ISR-producer /
         * task-consumer pattern. */
        context->rx_ring = dm_sw_ring_create(context->rx_ring_size,
                                              dm_sw_ring_flags_drop_old_data | context->rx_ring_wait_flags);
        if (context->rx_ring == NULL)
        {
            DMOD_LOG_ERROR("Failed to create RX ring buffer (size=%u)\n", context->rx_ring_size);
        }
        else
        {
            dmspi_port_set_rx_ring(context->config.instance, context->rx_ring);
        }
    }

    /* Register interrupt handler if ring buffer or dmhaman handler is active */
    if (context->rx_ring != NULL || context->interrupt_handler_name != NULL)
    {
        if (dmspi_port_add_interrupt_handler(context->config.instance,
                internal_interrupt_handler, context) != 0)
        {
            DMOD_LOG_ERROR("Failed to register interrupt handler\n");
        }
    }

    DMOD_LOG_INFO("SPI device created for instance %u\n", context->config.instance);

    /* Populate dev_num: minor = SPI instance number (1-based). */
    dev_num->flags = DMDRVI_NUM_MINOR;
    dev_num->major = 0;
    dev_num->minor = (dmdrvi_dev_id_t)context->config.instance;

    /* If the config uses a named section (e.g. [flash_spi]) populate alt_name
     * so the device filesystem registers the device under that human-friendly
     * name instead of a numeric path. */
    char section_buf[DMDRVI_ALT_NAME_MAX_LEN + 1];
    const char *section = detect_config_section(config, section_buf, sizeof(section_buf));
    if (strcmp(section, "dmspi") != 0)
    {
        size_t name_len = strlen(section);
        if (name_len <= DMDRVI_ALT_NAME_MAX_LEN)
        {
            dev_num->flags |= DMDRVI_NUM_ALT_NAME;
            memcpy(dev_num->alt_name, section, name_len + 1);
        }
    }

    return context;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, void, _free, ( dmdrvi_context_t context ))
{
    if (is_valid_context(context))
    {
        if (context->rx_ring != NULL || context->interrupt_handler_name != NULL)
            dmspi_port_remove_interrupt_handler(context->config.instance, context);

        if (context->rx_ring != NULL)
        {
            dmspi_port_set_rx_ring(context->config.instance, NULL);
            dm_sw_ring_destroy(context->rx_ring);
            context->rx_ring = NULL;
        }

        if (context->cs_lease != NULL)
        {
            cs_deassert(context);
            dmgpio_pin_release(context->cs_lease);
            context->cs_lease = NULL;
        }

        Dmod_Free(context->interrupt_handler_name);
        dmspi_port_deinit(context->config.instance);
        context->magic = 0;
        Dmod_Free(context);
    }
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t *dev_num ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmspi_dmdrvi_open\n");
        return NULL;
    }
    return context;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    /* No specific action needed to close the SPI device handle */
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, size_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, uint32_t offset ))
{
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;

    if (context->rx_ring != NULL)
        return (size_t)dm_sw_ring_read(context->rx_ring, buffer, (dm_sw_ring_capacity_t)size);

    if (context->config.role == dmspi_role_master)
        cs_assert(context);

    size_t received = 0;
    int ret = dmspi_port_receive(context->config.instance, (uint8_t *)buffer, size, &received);

    if (context->config.role == dmspi_role_master)
        cs_deassert(context);

    if (ret != 0)
        return 0;
    return received;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, size_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, uint32_t offset ))
{
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;

    if (context->config.role == dmspi_role_master)
        cs_assert(context);

    int ret = dmspi_port_transmit(context->config.instance, (const uint8_t *)buffer, size);

    if (context->config.role == dmspi_role_master)
        cs_deassert(context);

    if (ret != 0)
        return 0;
    return size;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    int ret = 0;
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmspi_dmdrvi_ioctl\n");
        return -EINVAL;
    }

    if (command >= dmspi_ioctl_cmd_max)
    {
        DMOD_LOG_ERROR("Invalid ioctl command %d\n", command);
        return -EINVAL;
    }

    if (command == dmspi_ioctl_cmd_reconfigure)
    {
        dmspi_port_deinit(context->config.instance);
        return configure(context);
    }

    if (command == dmspi_ioctl_cmd_transfer)
    {
        if (arg == NULL) return -EINVAL;
        dmspi_transfer_t *xfer = (dmspi_transfer_t *)arg;

        if (context->config.role == dmspi_role_master)
            cs_assert(context);

        ret = dmspi_port_transfer(context->config.instance, xfer->tx, xfer->rx, xfer->size);

        if (context->config.role == dmspi_role_master)
            cs_deassert(context);

        return ret;
    }

    if (command == dmspi_ioctl_cmd_set_interrupt_handler)
    {
        if (arg == NULL)
        {
            dmspi_port_remove_interrupt_handler(context->config.instance, context);
            return 0;
        }
        return dmspi_port_add_interrupt_handler(
            context->config.instance,
            (dmspi_port_interrupt_handler_t)*(dmspi_interrupt_handler_t *)arg,
            context);
    }

    if (arg == NULL)
    {
        DMOD_LOG_ERROR("Null argument for ioctl command %d\n", command);
        return -EINVAL;
    }

    /* Try read first */
    ret = read_configuration(context, command, arg);
    if (ret != 0)
    {
        /* Not a read command - try write */
        dmspi_config_t new_config;
        memcpy(&new_config, &context->config, sizeof(dmspi_config_t));

        ret = update_configuration(&new_config, command, arg);
        if (ret == 0)
        {
            memcpy(&context->config, &new_config, sizeof(dmspi_config_t));
            dmspi_port_deinit(context->config.instance);
            ret = configure(context);
        }
    }

    return ret;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmspi_dmdrvi_flush\n");
        return -EINVAL;
    }

    /* Wait for any in-progress transfer to complete */
    while (dmspi_port_is_busy(context->config.instance))
    {
        /* busy-wait: SPI transfers are short, no yield primitive needed here */
    }
    return 0;
}

dmod_dmdrvi_dif_api_declaration(1.0, dmspi, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters in dmspi_dmdrvi_stat\n");
        return -EINVAL;
    }

    stat->size = 0; /* Stream/message device, no fixed size */
    stat->mode = 0666;
    return 0;
}
