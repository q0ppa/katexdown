find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

add_executable(previewlifecycletest
    previewlifecycletest.cpp
    ../src/previewwidget.cpp
    ../src/settings.cpp
    ../data/resources.qrc
)

target_include_directories(previewlifecycletest PRIVATE ../src)

target_link_libraries(previewlifecycletest PRIVATE
    Qt6::Test
    Qt6::Widgets
    Qt6::WebEngineWidgets
    KF6::TextEditor
    KF6::I18n
    KF6::ConfigCore
    KF6::SyntaxHighlighting
)

add_test(NAME previewlifecycletest COMMAND previewlifecycletest)

# Runs headless, and keeps its config out of the developer's own Kate settings.
set_tests_properties(previewlifecycletest PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/data"
)
