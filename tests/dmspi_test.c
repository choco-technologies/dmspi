#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmspi.h"

static dmspi_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmspi_create();
}

void dmod_test_teardown(void)
{
    dmspi_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmspi_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmspi_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmspi_is_valid(g_handle));
}

DMOD_TEST_STEP(dmspi_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmspi_destroy(NULL);
}
