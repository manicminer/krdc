include(KrdcTestEnvironment)

ecm_add_tests(
    connectionlaunchtest.cpp
    LINK_LIBRARIES Qt::Test krdccore
    NAME_PREFIX "krdc-"
)
krdc_isolate_tests(krdc-connectionlaunchtest)
