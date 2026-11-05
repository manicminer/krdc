# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: BSD-3-Clause

include(KrdcTestEnvironment)

if(NOT TARGET krdc_rdpplugin)
    return()
endif()

# Plugins KRDC must not load: as an older KRDC would have installed them, and
# one this build tree no longer builds. Each aborts the test when loaded.
set(stale_plugins "${CMAKE_CURRENT_BINARY_DIR}/stale-plugins")
foreach(stale stalerdpplugin staleversionplugin staletoggledplugin)
    add_library(${stale} MODULE ${stale}.cpp)
    target_link_libraries(${stale} KF6::CoreAddons)
    set_target_properties(${stale} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${stale_plugins}/krdc")
endforeach()
# Same plugin id as the real RDP plugin.
set_target_properties(stalerdpplugin PROPERTIES OUTPUT_NAME krdc_rdpplugin PREFIX "")
# Only copied into test layouts by the test.
set_target_properties(staletoggledplugin PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")

# A compatible plugin from outside this build.
set(external_plugins "${CMAKE_CURRENT_BINARY_DIR}/external-plugins")
add_library(externalplugin MODULE externalplugin.cpp)
target_link_libraries(externalplugin krdccore)
set_target_properties(externalplugin PROPERTIES OUTPUT_NAME krdc_externalplugin PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${external_plugins}/krdc")

ecm_add_test(
    krdcstartuptest.cpp
    TEST_NAME krdcstartuptest
    NAME_PREFIX "krdc-"
    LINK_LIBRARIES Qt::Test krdc_objects
)
target_compile_definitions(krdcstartuptest PRIVATE
    KRDC_BINARY="$<TARGET_FILE:krdc>"
    KRDC_RDP_PLUGIN="$<TARGET_FILE:krdc_rdpplugin>"
    KRDC_CORE_LIBRARY="$<TARGET_FILE:krdccore>"
    KRDC_RELATIVE_PLUGINDIR="${KRDC_RELATIVE_PLUGINDIR}"
    STALE_TOGGLED_PLUGIN="$<TARGET_FILE:staletoggledplugin>"
    EXTERNAL_PLUGIN="$<TARGET_FILE:externalplugin>"
)
add_dependencies(krdcstartuptest krdc stalerdpplugin staleversionplugin staletoggledplugin externalplugin)

krdc_isolate_tests(krdc-krdcstartuptest)
set_property(TEST krdc-krdcstartuptest APPEND PROPERTY ENVIRONMENT "QT_PLUGIN_PATH=${stale_plugins}:${external_plugins}")
set_property(TEST krdc-krdcstartuptest PROPERTY TIMEOUT 180)
