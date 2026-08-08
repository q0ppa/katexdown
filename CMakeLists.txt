cmake_minimum_required(VERSION 3.16)

project(katdown VERSION 0.1.1 LANGUAGES CXX)

set(QT_MIN_VERSION 6.5.0)
set(KF_MIN_VERSION 6.0.0)

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    WebEngineWidgets
)

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    TextEditor
    CoreAddons
    I18n
    Config
    XmlGui
    SyntaxHighlighting
)

kcoreaddons_add_plugin(katdown
    INSTALL_NAMESPACE "kf6/ktexteditor"
    SOURCES
        src/plugin.cpp
        src/pluginview.cpp
        src/previewwidget.cpp
        src/configpage.cpp
        src/settings.cpp
        data/resources.qrc
)

target_include_directories(katdown PRIVATE src)

target_compile_definitions(katdown PRIVATE
    KATDOWN_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(katdown PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::WebEngineWidgets
    KF6::TextEditor
    KF6::CoreAddons
    KF6::I18n
    KF6::ConfigCore
    KF6::XmlGui
    KF6::SyntaxHighlighting
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
