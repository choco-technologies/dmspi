# ======================================================================
#               dmspi_port Module Configuration
# ======================================================================

# Name of the module
set(DMOD_MODULE_NAME        dmspi_port)

# Version is already set above and used in project()

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the DMR file for this module
set(DMOD_DMR_PATH           ${CMAKE_SOURCE_DIR}/dmspi_port.dmr)

# Determine common source files based on CPU family. Every STM32 family
# shares the same SPI register layout (see src/port/stm32_common), so this
# is where a new stm32-family port plugs into the shared implementation
# instead of duplicating it.
set(COMMON_SOURCES "")
if(DMOD_CPU_FAMILY MATCHES "^stm32")
    set(COMMON_SOURCES stm32_common/stm32_common.c)
endif()

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    ${DMOD_CPU_FAMILY}/port.c
    ${COMMON_SOURCES}
)

dmod_link_modules(${DMOD_MODULE_NAME}
    dm_sw_ring
    dmclk_port
    dmosi
)

target_include_directories(${DMOD_MODULE_NAME} BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)
