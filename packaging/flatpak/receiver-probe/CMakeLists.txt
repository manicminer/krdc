# SPDX-FileCopyrightText: 2026 KRDC contributors
# SPDX-License-Identifier: CC0-1.0
# Test only: KRDC's sign-in receiver sources (KRDC_SOURCE) built with the app's SDK.
cmake_minimum_required(VERSION 3.16)
project(krdc-avd-receiver-probe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(KF6I18n REQUIRED)
set(avd ${KRDC_SOURCE}/rdp/avd)
add_executable(probe main.cpp ${avd}/avdcallback.h ${avd}/avdcallback.cpp ${avd}/avdcallbackchannel.cpp)
target_include_directories(probe PRIVATE ${KRDC_SOURCE}/rdp)
target_compile_definitions(probe PRIVATE TRANSLATION_DOMAIN="krdc")
target_link_libraries(probe Qt6::Core Qt6::Network KF6::I18n)
