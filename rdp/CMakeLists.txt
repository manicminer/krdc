# The channel of krdc-avd-signin-handler, which only needs Qt.
add_library(krdc_avdchannel STATIC avd/avdcallbackchannel.cpp)
set_target_properties(krdc_avdchannel PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(krdc_avdchannel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(krdc_avdchannel PUBLIC Qt::Core)

# Started by the desktop for ms-appx-web: links once the user chose so; see doc/azure-virtual-desktop.md.
add_executable(krdc-avd-signin-handler avd/avdsigninhandler.cpp)
target_link_libraries(krdc-avd-signin-handler krdc_avdchannel Qt::Network)
install(TARGETS krdc-avd-signin-handler DESTINATION ${KDE_INSTALL_LIBEXECDIR})
if(KRDC_AVD_PACKAGED_HANDLER)
    # Named after the application ID, as Flatpak only exports such desktop files. The command is
    # looked up in the package's PATH (Flatpak runs it in a new sandbox of the same application).
    set(KRDC_AVD_PACKAGED_HANDLER_DESKTOP_FILE "${KRDC_APP_ID}.avd-signin-handler.desktop")
    configure_file(avd/avd-signin-handler.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${KRDC_AVD_PACKAGED_HANDLER_DESKTOP_FILE} @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${KRDC_AVD_PACKAGED_HANDLER_DESKTOP_FILE} DESTINATION ${KDE_INSTALL_APPDIR})
    configure_file(avd/krdc-avd-signin-handler-launcher.in ${CMAKE_CURRENT_BINARY_DIR}/krdc-avd-signin-handler-launcher @ONLY)
    install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/krdc-avd-signin-handler-launcher DESTINATION ${KDE_INSTALL_BINDIR} RENAME krdc-avd-signin-handler)
endif()

# Code without widgets, shared by the plugin and its tests.
add_library(krdc_rdpcore STATIC)
set_target_properties(krdc_rdpcore PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(krdc_rdpcore PRIVATE
    rdpaccesstoken.cpp
    rdpcredentials.cpp
    rdplaunchprofile.cpp
    rdpuihandoff.cpp
    avd/avdcallback.cpp
    avd/avdendpoints.cpp
    avd/avdfeed.cpp
    avd/avdfeedclient.cpp
    avd/avdhandlerregistration.cpp
    avd/avdoauth.cpp
    avd/avdtokenstore.cpp
    avd/avdworkspacecache.cpp
)

ecm_qt_declare_logging_category(krdc_rdpcore
    HEADER krdc_rdpcore_debug.h
    IDENTIFIER KRDC_RDP
    CATEGORY_NAME KRDC.rdp
)

target_compile_definitions(krdc_rdpcore PRIVATE TRANSLATION_DOMAIN=\"krdc\"
    KRDC_AVD_HANDLER_INSTALL_PATH=\"${KDE_INSTALL_FULL_LIBEXECDIR}/krdc-avd-signin-handler\")
if(KRDC_AVD_PACKAGED_HANDLER)
    target_compile_definitions(krdc_rdpcore PRIVATE KRDC_AVD_PACKAGED_HANDLER_DESKTOP_FILE=\"${KRDC_AVD_PACKAGED_HANDLER_DESKTOP_FILE}\")
endif()
target_include_directories(krdc_rdpcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})

target_link_libraries(krdc_rdpcore
    PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Network
    krdccore
    freerdp
    freerdp-client
    winpr
    krdc_avdchannel
    PRIVATE
    Qt6Keychain::Qt6Keychain
    KF6::I18n
    KF6::Service
)

krdc_add_plugin(krdc_rdpplugin)
# The plugin registers the helper from the build tree; building krdc builds the plugins, and so the helper.
add_dependencies(krdc_rdpplugin krdc-avd-signin-handler)

target_sources(krdc_rdpplugin PRIVATE
    rdpcliprdr.cpp
    rdpdisp.cpp
    rdpgraphics.cpp
    rdpmonitorview.cpp
    rdphostpreferences.cpp
    rdpviewfactory.cpp
    rdpview.cpp
    rdpsession.cpp
    sharedfolderfield.cpp
    avd/avdsignindialog.cpp
    avd/avdworkspacedialog.cpp
    avd/avdresourceview.cpp
)

ki18n_wrap_ui(krdc_rdpplugin
    rdppreferences.ui
)

ecm_qt_declare_logging_category(krdc_rdpplugin
    HEADER krdc_debug.h
    IDENTIFIER KRDC
    CATEGORY_NAME KRDC
)

target_include_directories(krdc_rdpplugin PUBLIC ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})

target_link_libraries(krdc_rdpplugin
    Qt::Core
    Qt::Gui
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::KCMUtils
    KF6::WindowSystem
    KF6::XmlGui
    KF6::KIOWidgets
    Threads::Threads
    Qt::Network
    krdc_rdpcore
    krdccore
    freerdp
    freerdp-client
    winpr
)

if(KF6Screen_FOUND)
    target_link_libraries(krdc_rdpplugin KF6::Screen)
    target_compile_definitions(krdc_rdpplugin PRIVATE HAVE_KSCREEN)
endif()

if(Qt6DBus_FOUND)
    target_link_libraries(krdc_rdpplugin Qt::DBus)
    target_compile_definitions(krdc_rdpplugin PRIVATE HAVE_DBUS)
endif()

krdc_add_kcm(kcm_krdc_rdpplugin)

target_sources(kcm_krdc_rdpplugin PRIVATE
    rdppreferences.cpp
    sharedfolderfield.cpp
)

target_link_libraries(kcm_krdc_rdpplugin
    Qt::Core
    Qt::Gui
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::KCMUtils
    KF6::XmlGui
    KF6::KIOWidgets
    krdccore
)

if(HAIKU)
    target_link_libraries(kcm_krdc_rdpplugin network)
endif()

# Sharing removable drives as they are plugged in replaces the drive check of FreeRDP 3.16.
if(KF6Solid_FOUND AND FreeRDP_VERSION VERSION_GREATER_EQUAL 3.16)
    target_sources(krdc_rdpplugin PRIVATE rdpremovabledrives.cpp)
    target_link_libraries(krdc_rdpplugin KF6::Solid)
    target_compile_definitions(krdc_rdpplugin PRIVATE HAVE_RDPDR_HOTPLUG)
    target_compile_definitions(kcm_krdc_rdpplugin PRIVATE HAVE_RDPDR_HOTPLUG)
endif()

add_dependencies(kcm_krdc_rdpplugin krdc_rdpplugin)

install(TARGETS kcm_krdc_rdpplugin DESTINATION ${KDE_INSTALL_PLUGINDIR}/krdc/kcms)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

# install(FILES smb2rdc.desktop DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus)
