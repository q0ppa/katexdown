find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

add_executable(previewlifecycletest
    previewlifecycletest.cpp
    ../src/previewwidget.cpp
    ../src/settings.cpp
    ../data/resources.qrc
)

add_executable(renderfeaturestest
    renderfeaturestest.cpp
    ../src/previewwidget.cpp
    ../src/settings.cpp
    ../data/resources.qrc
)

# Pins the renderer-memory maintenance: bounded RSS across document switches,
# the same-document recycle, and image survival across a recycle.
add_executable(rendermemorytest
    rendermemorytest.cpp
    ../src/previewwidget.cpp
    ../src/settings.cpp
    ../data/resources.qrc
)

# Exercises PluginView + PreviewWidget against a fake kate main-window host.
add_executable(followmodetest
    followmodetest.cpp
    ../src/pluginview.cpp
    ../src/previewwidget.cpp
    ../src/settings.cpp
    ../data/resources.qrc
)

foreach(tgt rendermemorytest previewlifecycletest renderfeaturestest followmodetest)
    target_include_directories(${tgt} PRIVATE ../src)

    target_link_libraries(${tgt} PRIVATE
        Qt6::Test
        Qt6::Widgets
        Qt6::WebEngineWidgets
        KF6::TextEditor
        KF6::I18n
        KF6::ConfigCore
        KF6::XmlGui
        KF6::SyntaxHighlighting
    )

    add_test(NAME ${tgt} COMMAND ${tgt})

    # Runs headless, keeps its config out of the developer's own Kate settings,
    # and points the data dir at the runtime cache fetched by tools/fetch-assets.py.
    set_tests_properties(${tgt} PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/xdg/data;KATEXDOWN_DATA_DIR=${CMAKE_SOURCE_DIR}/runtime"
    )
endforeach()

# Pure environment-flag logic (kdxchromiumflags.h): no QtWebEngine involved,
# so this binary needs no widget/engine stack and runs instantly.
add_executable(envflagstest
    envflagstest.cpp
)
target_include_directories(envflagstest PRIVATE ../src)
target_link_libraries(envflagstest PRIVATE Qt6::Test)
add_test(NAME envflagstest COMMAND envflagstest)
set_tests_properties(envflagstest PROPERTIES TIMEOUT 60)
