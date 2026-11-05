include(KrdcTestEnvironment)

ecm_add_tests(
    rdplaunchprofiletest.cpp
    rdpcredentialstest.cpp
    rdpaccesstokentest.cpp
    avdfeedtest.cpp
    avdoauthtest.cpp
    rdpuihandofftest.cpp
    LINK_LIBRARIES Qt::Test Qt::Network krdc_rdpcore
    NAME_PREFIX "krdc-"
)

ecm_add_test(
    avdworkspacecachetest.cpp
    TEST_NAME avdworkspacecachetest
    NAME_PREFIX "krdc-"
    LINK_LIBRARIES Qt::Test Qt::Gui Qt::Network krdc_rdpcore
)

ecm_add_tests(
    avdcallbacktest.cpp
    avdhandlerregistrationtest.cpp
    LINK_LIBRARIES Qt::Test Qt::Network KF6::Service KF6::ConfigCore krdc_rdpcore
    NAME_PREFIX "krdc-"
)

ecm_add_test(
    avdsignindialogtest.cpp
    ../avd/avdsignindialog.cpp
    ../avd/avdworkspacedialog.cpp
    ../avd/avdresourceview.cpp
    TEST_NAME avdsignindialogtest
    NAME_PREFIX "krdc-"
    LINK_LIBRARIES Qt::Test Qt::Widgets KF6::I18n KF6::WidgetsAddons KF6::KIOWidgets KF6::Service krdc_rdpcore
)

target_compile_definitions(avdsignindialogtest PRIVATE TRANSLATION_DOMAIN="krdc")

ecm_add_test(
    avdworkspacedialogtest.cpp
    ../avd/avdsignindialog.cpp
    ../avd/avdworkspacedialog.cpp
    ../avd/avdresourceview.cpp
    TEST_NAME avdworkspacedialogtest
    NAME_PREFIX "krdc-"
    LINK_LIBRARIES Qt::Test Qt::Widgets KF6::I18n KF6::WidgetsAddons KF6::KIOWidgets KF6::Service krdc_rdpcore
)
target_compile_definitions(avdworkspacedialogtest PRIVATE TRANSLATION_DOMAIN="krdc")

# These run the built krdc-avd-signin-handler as the desktop would.
foreach(test avdcallbacktest avdhandlerregistrationtest avdsignindialogtest avdworkspacedialogtest)
    target_compile_definitions(${test} PRIVATE KRDC_AVD_HANDLER_PATH="$<TARGET_FILE:krdc-avd-signin-handler>")
    add_dependencies(${test} krdc-avd-signin-handler)
endforeach()

ecm_add_test(
    rdpplugintest.cpp
    TEST_NAME rdpplugintest
    NAME_PREFIX "krdc-"
    LINK_LIBRARIES Qt::Test Qt::Widgets KF6::CoreAddons KF6::ConfigCore krdccore
)
target_compile_definitions(rdpplugintest PRIVATE KRDC_RDP_PLUGIN_PATH="$<TARGET_FILE:krdc_rdpplugin>")
add_dependencies(rdpplugintest krdc_rdpplugin)

krdc_isolate_tests(
    krdc-rdpplugintest
    krdc-rdplaunchprofiletest
    krdc-rdpcredentialstest
    krdc-rdpaccesstokentest
    krdc-avdfeedtest
    krdc-avdoauthtest
    krdc-avdsignindialogtest
    krdc-avdworkspacedialogtest
    krdc-avdworkspacecachetest
    krdc-avdcallbacktest
    krdc-avdhandlerregistrationtest
    krdc-rdpuihandofftest
)
