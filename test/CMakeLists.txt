if(NOT NDEBUG)
    set(BUILD_TEST true)
endif()

if(NOT BUILD_TEST)
    return()
endif()

krdc_add_plugin(krdc_testplugin)

target_sources(krdc_testplugin PRIVATE
    testviewfactory.cpp
    testview.cpp
)

target_link_libraries(krdc_testplugin
    Qt::Core
    KF6::ConfigCore
    KF6::I18n
    KF6::CoreAddons
    Qt::Gui
    krdccore
)
