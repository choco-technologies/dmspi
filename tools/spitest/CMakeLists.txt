# =====================================================================
#               spitest - Manual dmspi Device Test Tool
# =====================================================================
# Added via add_subdirectory() from the top-level CMakeLists.txt, so it
# shares dmspi's own dmod fetch/configuration (DMOD_MODULE_NAME, DMOD_DIR,
# DMOD_MODULE_VERSION, ...) instead of setting up a second, separate DMOD
# project - see dmtty/tools/tty for the same pattern elsewhere in the DMOD
# ecosystem.

# Path to the DMR file for this module - setting this before dmod_add_executable
# makes the build automatically produce a ready-to-publish release package
# (via mkdmrpkg) under build/packages/spitest.zip, right alongside dmspi.zip
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/spitest.dmr)

dmod_add_executable(spitest ${DMOD_MODULE_VERSION}
    main.c
)

# Device access goes through Dmod_FileOpen/_Ioctl/_FileClose - the portable
# DMOD SAL wrappers around dmvfs, declared in dmod's own core headers
# (dmod_sal.h, pulled in by dmod.h). dmvfs itself is a DMOD_SYSTEM
# component built into dmod-boot, not a separately loadable module, so
# there is nothing to dmod_link_modules() for it.
#
# dmosi is a separate module - fetch its headers for the worker thread
# used by the loopback test. The real implementation is resolved at
# runtime against the system's own instance, same as dmtty/tools/tty's
# dmtty_attach() - no static linkage.
dmod_link_modules(spitest
    dmosi
)

# dmspi_types.h lives in this same repo (no dmf-get fetch needed) - it only
# pulls in dmspi_ioctl_cmd_t/dmspi_transfer_t/dmspi_role_t, not the full
# driver API, since every device access here goes through Dmod_FileOpen by
# path.
target_include_directories(spitest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
