#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmspi.h"
#include "dmdrvi.h"
#include "dmini.h"
#include <errno.h>
#include <string.h>

static dmspi_config_t g_config;

void dmod_test_setup(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.instance = 1;
    g_config.role     = dmspi_role_master;
    g_config.baudrate = 1000000;
}

void dmod_test_teardown(void)
{
}

DMOD_TEST_STEP(dmspi_validate_config_accepts_valid_master)
{
    DMOD_TEST_EXPECT_TRUE(dmspi_validate_config(&g_config));
}

DMOD_TEST_STEP(dmspi_validate_config_rejects_zero_instance)
{
    g_config.instance = 0;
    DMOD_TEST_EXPECT_FALSE(dmspi_validate_config(&g_config));
}

DMOD_TEST_STEP(dmspi_validate_config_rejects_master_without_baudrate)
{
    g_config.baudrate = 0;
    DMOD_TEST_EXPECT_FALSE(dmspi_validate_config(&g_config));
}

DMOD_TEST_STEP(dmspi_validate_config_accepts_slave_without_baudrate)
{
    g_config.role     = dmspi_role_slave;
    g_config.baudrate = 0;
    DMOD_TEST_EXPECT_TRUE(dmspi_validate_config(&g_config));
}

DMOD_TEST_STEP(dmspi_validate_config_rejects_null)
{
    DMOD_TEST_EXPECT_FALSE(dmspi_validate_config(NULL));
}

/* ---- ioctl numbering, through the dmdrvi DIF the way dmdevfs calls it ---- */

typedef struct
{
    dmod_dmdrvi_create_t    create;
    dmod_dmdrvi_free_t      free;
    dmod_dmdrvi_open_t      open;
    dmod_dmdrvi_close_t     close;
    dmod_dmdrvi_ioctl_t     ioctl;
} driver_t;

static bool get_driver(driver_t* drv)
{
    Dmod_Context_t* module = Dmod_GetModuleContext("dmspi");
    if (module == NULL)
    {
        return false;
    }
    drv->create = Dmod_GetDifFunction(module, dmod_dmdrvi_create_sig);
    drv->free   = Dmod_GetDifFunction(module, dmod_dmdrvi_free_sig);
    drv->open   = Dmod_GetDifFunction(module, dmod_dmdrvi_open_sig);
    drv->close  = Dmod_GetDifFunction(module, dmod_dmdrvi_close_sig);
    drv->ioctl  = Dmod_GetDifFunction(module, dmod_dmdrvi_ioctl_sig);
    return drv->create != NULL && drv->free != NULL && drv->open != NULL &&
           drv->close != NULL && drv->ioctl != NULL;
}

DMOD_TEST_STEP(dmspi_ioctl_answers_only_its_own_command_range)
{
    driver_t drv = { 0 };
    DMOD_TEST_EXPECT_TRUE(get_driver(&drv));
    if (drv.ioctl == NULL)
    {
        return;
    }
    dmini_context_t ini = dmini_create();
    dmini_parse_string(ini, "[dmspi]\ninstance=1\nrole=master\nbaudrate=1000000\nmode=0\n");
    dmdrvi_dev_num_t dev_num = { 0 };
    dmdrvi_context_t ctx = drv.create(ini, &dev_num);
    DMOD_TEST_EXPECT_NOT_NULL(ctx);
    void* handle = (ctx != NULL) ? drv.open(ctx, DMDRVI_O_RDWR, &dev_num) : NULL;
    DMOD_TEST_EXPECT_NOT_NULL(handle);
    if (handle != NULL)
    {
        dmspi_role_t role = dmspi_role_slave;
        uint32_t probe[16] = { 0 };

        /* Standard dmdrvi commands - dmdevfs sends the first two to every node. */
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, DMDRVI_IOCTL_BLOCK_GET_INFO, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, DMDRVI_IOCTL_MONITOR_GET_POLICY, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, DMDRVI_IOCTL_NET_GET_MAC_ADDR, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, 0, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, -1, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, dmspi_ioctl_cmd_max, probe), -ENOTTY);

        /* dmspi's own commands, from DMDRVI_IOCTL_CUSTOM_BASE. */
        DMOD_TEST_EXPECT_EQ((int)dmspi_ioctl_cmd_get_role, DMDRVI_IOCTL_CUSTOM_BASE);
        DMOD_TEST_EXPECT_EQ(drv.ioctl(ctx, handle, dmspi_ioctl_cmd_get_role, &role), 0);
        DMOD_TEST_EXPECT_EQ(role, dmspi_role_master);
        drv.close(ctx, handle);
    }
    if (ctx != NULL)
    {
        drv.free(ctx);
    }
    dmini_destroy(ini);
}
