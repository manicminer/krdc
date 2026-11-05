# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: BSD-3-Clause

# Tests run offscreen with private XDG directories, so they neither read nor
# change the configuration, keychain or helpers of the user running them.
function(krdc_isolate_tests)
    foreach(test ${ARGN})
        set(home "${CMAKE_CURRENT_BINARY_DIR}/${test}.home")
        # Short, as socket paths in it are limited to about 100 bytes.
        string(MD5 runtime_id "${CMAKE_CURRENT_BINARY_DIR}/${test}")
        string(SUBSTRING "${runtime_id}" 0 8 runtime_id)
        set(runtime "${CMAKE_BINARY_DIR}/rt/${runtime_id}")
        set_tests_properties(${test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;HOME=${home};XDG_CONFIG_HOME=${home}/config;XDG_DATA_HOME=${home}/data;XDG_CACHE_HOME=${home}/cache;XDG_STATE_HOME=${home}/state;XDG_RUNTIME_DIR=${runtime};DBUS_SESSION_BUS_ADDRESS=disabled:;QT_LOGGING_RULES=*.debug=false;BROWSER=/bin/false"
        )
        file(MAKE_DIRECTORY "${home}/config" "${home}/data" "${home}/cache" "${home}/state" "${runtime}")
        file(CHMOD "${CMAKE_BINARY_DIR}/rt" "${runtime}" DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
endfunction()
