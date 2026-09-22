#include "dmod.h"
#include "dmspi_types.h"
#include "dmosi.h"
#include <string.h>

/**
 * @brief Manual dmspi device test tool.
 *
 * Exercises a real SPI transfer against an already-configured dmspi device
 * node - it does not configure or create anything itself. The device is
 * whatever `dmdevfs` set up from a dmspi config.ini (see
 * ../../configs/README.md); this tool only needs the resulting path
 * (e.g. /dev/dmspi1) and reads the device's role (master/slave) back from
 * it via ioctl.
 *
 * Two distinct, deterministic byte patterns are used - one MOSI-bound
 * (master->slave), one MISO-bound (slave->master) - so a mismatch (or a
 * MOSI/MISO swap) is caught, not just "some bytes moved".
 */

#define TEST_PATTERN_LEN     16U
#define SLAVE_READY_DELAY_MS 20U

static void build_mosi_pattern(uint8_t *buf)
{
    for (unsigned i = 0; i < TEST_PATTERN_LEN; i++)
        buf[i] = (uint8_t)i;
}

static void build_miso_pattern(uint8_t *buf)
{
    for (unsigned i = 0; i < TEST_PATTERN_LEN; i++)
        buf[i] = (uint8_t)(0xFFU - i);
}

static bool verify_pattern(const char *label, const uint8_t *actual, const uint8_t *expected, size_t len)
{
    bool ok = true;
    for (size_t i = 0; i < len; i++)
    {
        if (actual[i] != expected[i])
        {
            if (ok)
                DMOD_LOG_ERROR("%s: mismatch\n", label);
            DMOD_LOG_ERROR("  byte %u: expected 0x%02X, got 0x%02X\n",
                (unsigned)i, expected[i], actual[i]);
            ok = false;
        }
    }
    if (ok)
        Dmod_Printf("%s: OK (%u bytes matched)\n", label, (unsigned)len);
    return ok;
}

static void *open_device(const char *path)
{
    /* Dmod_FileOpen/_Ioctl/_FileClose are the portable DMOD SAL wrappers
     * around dmvfs - they work the same way from any module without
     * fetching dmvfs itself (a DMOD_SYSTEM component built into dmod-boot,
     * not a separately loadable module). */
    void *fp = Dmod_FileOpen(path, "r+");
    if (fp == NULL)
        DMOD_LOG_ERROR("spitest: failed to open '%s'\n", path);
    return fp;
}

static bool query_role(void *fp, const char *path, dmspi_role_t *out_role)
{
    int ret = Dmod_Ioctl(fp, dmspi_ioctl_cmd_get_role, out_role);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("spitest: failed to query role of '%s' (error %d) - is it really a "
                        "dmspi device?\n", path, ret);
        return false;
    }
    return true;
}

static void run_transfer(void *fp, bool as_master, bool *ok)
{
    uint8_t tx[TEST_PATTERN_LEN];
    uint8_t rx[TEST_PATTERN_LEN] = {0};
    if (as_master)
        build_mosi_pattern(tx);
    else
        build_miso_pattern(tx);

    dmspi_transfer_t xfer = { .tx = tx, .rx = rx, .size = TEST_PATTERN_LEN };
    int ret = Dmod_Ioctl(fp, dmspi_ioctl_cmd_transfer, &xfer);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("spitest: transfer failed (error %d)%s\n", ret,
            as_master ? " - check wiring" : " - is the master running and wired correctly?");
        *ok = false;
        return;
    }

    uint8_t expected[TEST_PATTERN_LEN];
    if (as_master)
        build_miso_pattern(expected);
    else
        build_mosi_pattern(expected);

    *ok = verify_pattern(as_master ? "MISO (slave->master)" : "MOSI (master->slave)",
        rx, expected, TEST_PATTERN_LEN);
}

/* ---- single device: this board holds only one side of the link, the
 *      other side is a peer board wired to it ---- */

static int run_single(const char *path)
{
    void *fp = open_device(path);
    if (fp == NULL)
        return -1;

    dmspi_role_t role;
    if (!query_role(fp, path, &role))
    {
        Dmod_FileClose(fp);
        return -1;
    }

    bool as_master = (role == dmspi_role_master);
    Dmod_Printf("spitest: '%s' is configured as %s\n", path, as_master ? "master" : "slave");
    if (!as_master)
        Dmod_Printf("spitest: waiting for the peer board's master to clock data in...\n");

    bool ok = false;
    run_transfer(fp, as_master, &ok);
    Dmod_FileClose(fp);

    Dmod_Printf("\n=== %s test ('%s'): %s ===\n",
        as_master ? "Master" : "Slave", path, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---- two devices on this board: one worker thread per side, so both
 *      run their (blocking) transfer at the same time ---- */

typedef struct
{
    void *fp;
    bool as_master;
    bool ok;
} worker_ctx_t;

static void worker_thread_entry(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    run_transfer(ctx->fp, ctx->as_master, &ctx->ok);
}

static int run_loopback(const char *path_a, const char *path_b)
{
    void *fp_a = open_device(path_a);
    void *fp_b = open_device(path_b);
    if (fp_a == NULL || fp_b == NULL)
    {
        if (fp_a) Dmod_FileClose(fp_a);
        if (fp_b) Dmod_FileClose(fp_b);
        return -1;
    }

    dmspi_role_t role_a, role_b;
    if (!query_role(fp_a, path_a, &role_a) || !query_role(fp_b, path_b, &role_b))
    {
        Dmod_FileClose(fp_a);
        Dmod_FileClose(fp_b);
        return -1;
    }
    if (role_a == role_b)
    {
        DMOD_LOG_ERROR("spitest: '%s' and '%s' are both configured as %s - one must be "
                        "master and the other slave\n", path_a, path_b,
                        (role_a == dmspi_role_master) ? "master" : "slave");
        Dmod_FileClose(fp_a);
        Dmod_FileClose(fp_b);
        return -1;
    }

    void *master_fp = (role_a == dmspi_role_master) ? fp_a : fp_b;
    void *slave_fp  = (role_a == dmspi_role_master) ? fp_b : fp_a;
    const char *master_path = (role_a == dmspi_role_master) ? path_a : path_b;
    const char *slave_path  = (role_a == dmspi_role_master) ? path_b : path_a;
    Dmod_Printf("spitest: '%s' is master, '%s' is slave\n", master_path, slave_path);

    worker_ctx_t slave_ctx = { .fp = slave_fp, .as_master = false, .ok = false };
    dmosi_thread_t slave_thread = dmosi_thread_create(worker_thread_entry, &slave_ctx,
        1, 2048U + DMOSI_THREAD_STACK_OVERHEAD, "spitest_slave", dmosi_process_current());
    if (slave_thread == NULL)
    {
        DMOD_LOG_ERROR("spitest: failed to start slave worker thread\n");
        Dmod_FileClose(fp_a);
        Dmod_FileClose(fp_b);
        return -1;
    }

    /* Give the slave task a moment to reach its blocking transfer before the
     * master starts clocking, so it doesn't miss the first byte. */
    dmosi_thread_sleep(SLAVE_READY_DELAY_MS);

    bool master_ok = false;
    run_transfer(master_fp, true, &master_ok);

    dmosi_thread_join(slave_thread);

    Dmod_FileClose(fp_a);
    Dmod_FileClose(fp_b);

    bool ok = master_ok && slave_ctx.ok;
    Dmod_Printf("\n=== Loopback test ('%s' master <-> '%s' slave): %s ===\n",
        master_path, slave_path, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static void print_usage(const char *prog)
{
    Dmod_Printf("Usage: %s <device_path> [peer_device_path]\n", prog);
    Dmod_Printf("\n");
    Dmod_Printf("Exercises a real SPI transfer on an already-configured dmspi device -\n");
    Dmod_Printf("see ../../configs/README.md for how to get one via dmdevfs. The\n");
    Dmod_Printf("device's role (master/slave) is read back from it via ioctl, not\n");
    Dmod_Printf("passed on the command line.\n");
    Dmod_Printf("\n");
    Dmod_Printf("  <device_path>       Run against a single device (use on each board\n");
    Dmod_Printf("                      separately for a two-board master<->slave test).\n");
    Dmod_Printf("  [peer_device_path]  Also open a second device on THIS board and run a\n");
    Dmod_Printf("                      loopback test between the two - one must already be\n");
    Dmod_Printf("                      configured as master, the other as slave, and wired\n");
    Dmod_Printf("                      together with a cable.\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        print_usage(argv[0]);
        return (argc < 2) ? 1 : 0;
    }

    if (argc >= 3)
        return (run_loopback(argv[1], argv[2]) == 0) ? 0 : 1;

    return (run_single(argv[1]) == 0) ? 0 : 1;
}
