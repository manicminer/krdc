cmake_minimum_required(VERSION 3.16)

set (RELEASE_SERVICE_VERSION_MAJOR "26")
set (RELEASE_SERVICE_VERSION_MINOR "11")
set (RELEASE_SERVICE_VERSION_MICRO "70")

set (RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(krdc VERSION ${RELEASE_SERVICE_VERSION})

set (QT_MIN_VERSION "6.6.0")
set (QT_KEYCHAIN_MIN_VERSION "0.15.0")
set (KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMSetupVersion)
include(ECMQtDeclareLoggingCategory)
include(FeatureSummary)
include(ECMDeprecationSettings)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMInstallIcons)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Network)
if (Qt6Gui_VERSION VERSION_GREATER_EQUAL "6.10.0")
    find_package(Qt6GuiPrivate ${QT_MIN_VERSION} REQUIRED NO_MODULE)
endif()
if (UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Qt6WaylandClient ${QT_MIN_VERSION} CONFIG)
    set(HAVE_WAYLAND ${Qt6WaylandClient_FOUND})
    find_package(Qt6DBus ${QT_MIN_VERSION} CONFIG)
endif()

find_package(Qt6Keychain ${QT_KEYCHAIN_MIN_VERSION} REQUIRED)

if(BUILD_TESTING)
    find_package(Qt6Test ${QT_MIN_VERSION} CONFIG REQUIRED)
    include(ECMAddTests)
endif()

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    Config
    CoreAddons
    KCMUtils
    DNSSD
    NotifyConfig
    Notifications
    Bookmarks
    IconThemes
    XmlGui
    Completion
    WidgetsAddons
    WindowSystem
    NotifyConfig
    I18n
    KIO
    Service
    Crash
    GuiAddons
)

find_package(KF6StatusNotifierItem ${KF_MIN_VERSION} REQUIRED)

find_package(KF6DocTools)

find_package(Threads REQUIRED)

# use sane compile flags
add_definitions(
  -DQT_USE_QSTRINGBUILDER
  -DQT_NO_CAST_TO_ASCII
  -DQT_NO_CAST_FROM_ASCII
  -DQT_STRICT_ITERATORS
  -DQT_NO_URL_CAST_FROM_STRING
  -DQT_NO_CAST_FROM_BYTEARRAY
  -DQT_NO_KEYWORDS
  -DQT_NO_FOREACH
)
ecm_setup_version(${RELEASE_SERVICE_VERSION}
    VARIABLE_PREFIX KRDC
    SOVERSION 6
    VERSION_HEADER krdc_version.h
)

if(WIN32)
    set(CMAKE_REQUIRED_LIBRARIES ${KDEWIN32_LIBRARIES})
    set(CMAKE_REQUIRED_INCLUDES  ${KDEWIN32_INCLUDES})
endif(WIN32)

option(WITH_VNC "Enable VNC support." ON)
add_feature_info(VNC WITH_VNC "Building with support for the VNC protocol. Disable with -DWITH_VNC=NO")
option(WITH_RDP "Enable RDP support using FreeRDP" ON)
add_feature_info(RDP WITH_RDP "Building with support for the RDP protocol. Disable with -DWITH_RDP=NO")
option(WITH_SPICE "Enable SPICE support." ON)
add_feature_info(SPICE WITH_SPICE "Building with support for the SPICE protocol. Disable with -DWITH_SPICE=NO")

ecm_set_disabled_deprecation_versions(QT 6.8
     KF 6.12
    )


set(CMAKE_CXX_STANDARD 20)

find_package(LibSSH)
set_package_properties(LibSSH PROPERTIES
    DESCRIPTION "ssh library"
    URL "https://libssh.org/"
    PURPOSE "Needed to build SSH tunnel support "
    TYPE OPTIONAL
)

# KRDC first looks for plugins relative to its executable, where they get
# installed, so a build tree or another prefix uses its own plugins instead of
# those of an installed KRDC. The build tree mirrors that layout, as bin/krdc
# is taken by the executable.
foreach(dir BINDIR PLUGINDIR)
    if(IS_ABSOLUTE "${KDE_INSTALL_${dir}}")
        set(krdc_install_${dir} "${KDE_INSTALL_${dir}}")
    else()
        set(krdc_install_${dir} "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_${dir}}")
    endif()
endforeach()
file(RELATIVE_PATH KRDC_RELATIVE_PLUGINDIR "${krdc_install_BINDIR}" "${krdc_install_PLUGINDIR}")
get_filename_component(KRDC_BUILD_PLUGINDIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${KRDC_RELATIVE_PLUGINDIR}" ABSOLUTE)
string(FIND "${KRDC_BUILD_PLUGINDIR}/" "${CMAKE_BINARY_DIR}/" krdc_plugindir_in_build)
if(NOT krdc_plugindir_in_build EQUAL 0)
    # Plugins installed far from the executable: keep them in the build tree.
    set(KRDC_RELATIVE_PLUGINDIR "../plugins")
    get_filename_component(KRDC_BUILD_PLUGINDIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${KRDC_RELATIVE_PLUGINDIR}" ABSOLUTE)
endif()

function(krdc_add_plugin plugin)
    kcoreaddons_add_plugin(${plugin} INSTALL_NAMESPACE "krdc")
    set_target_properties(${plugin} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${KRDC_BUILD_PLUGINDIR}/krdc")
    set_property(GLOBAL APPEND PROPERTY KRDC_PLUGINS ${plugin})
    set_property(GLOBAL APPEND PROPERTY KRDC_VIEW_PLUGINS ${plugin})
endfunction()

# Configuration modules are found by path through the same plugin search.
function(krdc_add_kcm kcm)
    add_library(${kcm})
    set_target_properties(${kcm} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${KRDC_BUILD_PLUGINDIR}/krdc/kcms")
    set_property(GLOBAL APPEND PROPERTY KRDC_PLUGINS ${kcm})
endfunction()

add_subdirectory(core)

if(WITH_VNC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VNCCLIENT libvncclient>=0.9.15 QUIET IMPORTED_TARGET)
    if(VNCCLIENT_FOUND)
        add_definitions(-DSUPPORT_UTF8_CLIPBOARD=1)
        message(STATUS "Found libvncclient version ${VNCCLIENT_VERSION}")
    else()
        pkg_check_modules(VNCCLIENT libvncclient REQUIRED IMPORTED_TARGET)
        add_definitions(-DSUPPORT_UTF8_CLIPBOARD=0)
        message(WARNING "UTF-8 clipboard requires libvncclient>=0.9.15 (found ${VNCCLIENT_VERSION})")
    endif()

    add_subdirectory(vnc)
endif()

if(WITH_RDP)
    # Workaround https://github.com/FreeRDP/FreeRDP/issues/11072
    if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD|DragonFly")
        list(APPEND CMAKE_PREFIX_PATH /usr/local/share/cmake/Modules/FreeRDP3)
        list(APPEND CMAKE_PREFIX_PATH /usr/local/share/cmake/Modules/FreeRDP-Client3)
        list(APPEND CMAKE_PREFIX_PATH /usr/local/share/cmake/Modules/WinPR3)
    endif()

    # Tested baseline for the Azure Virtual Desktop support, not the earliest version that may work.
    find_package(FreeRDP 3.32.2 REQUIRED)
    find_package(WinPR 3 REQUIRED)
    find_package(FreeRDP-Client 3.32.2 REQUIRED)
    find_package(KF6Screen ${KF_MIN_VERSION})
    find_package(KF6Solid ${KF_MIN_VERSION})
    set_package_properties(KF6Solid PROPERTIES
        PURPOSE "Sharing removable drives with RDP sessions, with FreeRDP 3.16 or later"
        TYPE OPTIONAL
    )

    MESSAGE(STATUS "Found FreeRDP version ${FreeRDP_VERSION_MAJOR}.${FreeRDP_VERSION_MINOR}.${FreeRDP_VERSION_REVISION}")

    set_package_properties(FreeRDP PROPERTIES
        DESCRIPTION "A free Remote Desktop Protocol (RDP) Implementation"
        URL "https://www.freerdp.com/"
        PURPOSE "FreeRDP provides RDP support"
        TYPE REQUIRED
    )
    set_package_properties(WinPR PROPERTIES
        DESCRIPTION "Windows Portable Runtime"
        URL "https://www.freerdp.com/"
        PURPOSE "Runtime used by FreeRDP"
        TYPE REQUIRED
    )
    set_package_properties(FreeRDP-Client PROPERTIES
        DESCRIPTION "A free Remote Desktop Protocol (RDP) Implementation, Client libraries"
        URL "https://www.freerdp.com/"
        PURPOSE "FreeRDP provides RDP support"
        TYPE REQUIRED
    )

    add_subdirectory(rdp)
endif()

if(WITH_SPICE)
    find_package(Glib REQUIRED)
    find_package(Spiceglib REQUIRED)
    add_subdirectory(spice)
endif()

add_subdirectory(test)

if(KF6DocTools_FOUND)
    add_subdirectory(doc)
else()
    message("KF6DocTools not found. Not building documentation.")
endif()


# Everything but main(), so autotests can build the real main window.
add_library(krdc_objects OBJECT)

target_compile_definitions(krdc_objects PUBLIC -DBUILD_ZEROCONF)

target_sources(krdc_objects PRIVATE
    config/hostpreferenceslist.cpp
    config/preferencesdialog.cpp
    floatingtoolbar.cpp
    bookmarkmanager.cpp
    connectiondelegate.cpp
    connectionstatuswidget.cpp
    remotedesktopsmodel.cpp
    systemtrayicon.cpp
    tabbedviewwidget.cpp
    factorwidget.cpp
    mainwindow.cpp

    krdc.qrc
)

ki18n_wrap_ui(krdc_objects
    config/general.ui
)

ecm_qt_declare_logging_category(krdc_objects
    HEADER krdc_debug.h
    IDENTIFIER KRDC
    CATEGORY_NAME KRDC
    DESCRIPTION "KRDC"
    EXPORT KRDC
)

target_link_libraries(krdc_objects PUBLIC
    Qt::Network
    KF6::Bookmarks
    KF6::BookmarksWidgets
    KF6::Completion
    KF6::ConfigCore
    KF6::Crash
    KF6::DNSSD
    KF6::IconThemes
    KF6::KCMUtils
    KF6::Notifications
    KF6::NotifyConfig
    KF6::StatusNotifierItem
    KF6::WidgetsAddons
    KF6::XmlGui
    krdccore
)
target_include_directories(krdc_objects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

add_executable(krdc main.cpp)
target_link_libraries(krdc krdc_objects)
# Building krdc alone, as IDE run targets do, must also build the plugins it loads.
get_property(krdc_plugins GLOBAL PROPERTY KRDC_PLUGINS)
if(krdc_plugins)
    add_dependencies(krdc ${krdc_plugins})
endif()
# Marks the build tree for krdc: it loads the plugins listed here, and no
# others, such as those of protocols no longer built or of an installed KRDC.
# Not installed; an installed KRDC searches the plugin path.
get_property(krdc_view_plugins GLOBAL PROPERTY KRDC_VIEW_PLUGINS)
set(krdc_build_manifest "")
foreach(plugin ${krdc_view_plugins})
    string(APPEND krdc_build_manifest "${KRDC_RELATIVE_PLUGINDIR}/krdc/$<TARGET_FILE_NAME:${plugin}>\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/krdc-build-plugins" CONTENT "${krdc_build_manifest}")

install(TARGETS krdc ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

configure_file(org.kde.krdc.desktop.cmake ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdc.desktop @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.krdc.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES org.kde.krdc.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR})

ecm_install_icons(
    ICONS sc-apps-krdc.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}
)

find_package(SharedMimeInfo)
if(SharedMimeInfo_FOUND)
    install(FILES org.kde.krdc-mime.xml DESTINATION ${KDE_INSTALL_MIMEDIR})
    update_xdg_mimetypes(${KDE_INSTALL_MIMEDIR})
endif()

ki18n_install(po)
if(KF6DocTools_FOUND)
    kdoctools_install(po)
endif()

ecm_qt_install_logging_categories(
    EXPORT KRDC
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})

kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
