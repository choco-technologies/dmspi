#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmspi.h"
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
