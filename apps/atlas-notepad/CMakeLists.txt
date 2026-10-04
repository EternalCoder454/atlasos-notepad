cmake_minimum_required(VERSION 3.24)
project(atlas-notepad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "" FORCE)
endif()
include(GNUInstallDirs)
include(FetchContent)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Concurrent DBus Gui PrintSupport Qml Quick QuickControls2 Widgets)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)
find_package(KF6DBusAddons REQUIRED)
find_package(KF6WindowSystem REQUIRED)
find_package(Corrosion REQUIRED)

set(WORKSPACE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# Rust: the Markdown reader and the file layer.
corrosion_import_crate(MANIFEST_PATH ${WORKSPACE_DIR}/Cargo.toml
    CRATES atlas-notepad LOCKED)

# Atlas.Ui comes from the atlasos-updater commit pinned for atlas-core in the
# workspace Cargo.toml, so the two can't drift apart. To try unpushed Atlas.Ui
# changes, point CMake at a local checkout (scripts/dev.sh mounts $ATLAS_UI):
#   -DFETCHCONTENT_SOURCE_DIR_ATLASOS_UPDATER=/atlas-ui
file(READ ${WORKSPACE_DIR}/Cargo.toml _workspace_toml)
string(REGEX MATCH "atlas-core = {[^}]*rev = \"([0-9a-f]+)\"" _pin "${_workspace_toml}")
set(ATLAS_UPDATER_REV "${CMAKE_MATCH_1}")
string(LENGTH "${ATLAS_UPDATER_REV}" _rev_len)
if(NOT _rev_len EQUAL 40)
    message(FATAL_ERROR "No full 40-character atlas-core rev in ${WORKSPACE_DIR}/Cargo.toml")
endif()
message(STATUS "Atlas.Ui from atlasos-updater ${ATLAS_UPDATER_REV}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WORKSPACE_DIR}/Cargo.toml)
FetchContent_Declare(atlasos_updater
    GIT_REPOSITORY https://github.com/EternalCoder454/atlasos-updater.git
    GIT_TAG ${ATLAS_UPDATER_REV}
    SOURCE_SUBDIR ui)
FetchContent_MakeAvailable(atlasos_updater)

# The app's QObjects (cpp/app.h), shared with app_test.
set(APP_SOURCES
    cpp/app.h cpp/sizelimits.h cpp/notepad_core.h
    cpp/settings.cpp cpp/document.cpp cpp/documentlist.cpp cpp/app.cpp
    cpp/session.h cpp/document_p.h cpp/session.cpp cpp/linenumbers.cpp)
set(APP_LIBS
    atlas_notepad
    Qt6::Core Qt6::Concurrent Qt6::DBus Qt6::Gui Qt6::PrintSupport Qt6::Qml Qt6::Quick
    Qt6::QuickControls2 Qt6::Widgets KF6::DBusAddons KF6::WindowSystem)

qt_add_executable(atlas-notepad-app cpp/main.cpp cpp/bench.h cpp/bench.cpp)
set_target_properties(atlas-notepad-app PROPERTIES OUTPUT_NAME atlas-notepad)
target_compile_definitions(atlas-notepad-app PRIVATE ATLAS_NOTEPAD_VERSION="${PROJECT_VERSION}")
# The generated QML type registrations include cpp/markdown.h by name.
target_include_directories(atlas-notepad-app PRIVATE cpp)

# qmlcachegen compiles every QML file to C++ at build time; the app reads no
# .qml from disk at run time.
qt_add_qml_module(atlas-notepad-app
    URI net.eterneon.atlas.notepad
    VERSION 1.0
    # Atlas.Ui is linked explicitly (atlas-uiplugin); Kirigami and Qt load
    # their plugins at run time.
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml qml/EditorView.qml qml/FormatToolbar.qml qml/GlobalMenu.qml
        qml/FallbackMenu.qml qml/SettingsPage.qml qml/UnsavedDialog.qml qml/SlimScrollBar.qml
    SOURCES
        cpp/markdown.h cpp/highlighter.cpp cpp/markdowneditor.cpp cpp/decorations.cpp
        ${APP_SOURCES}
    RESOURCES
        icons/bold.svg icons/bullet-list.svg icons/checklist.svg icons/chevron-down.svg
        icons/chevron-right.svg icons/chevron-up.svg icons/close.svg icons/code.svg
        icons/copy.svg icons/divider.svg icons/find-replace.svg icons/heading1.svg
        icons/heading2.svg icons/image.svg icons/italic.svg icons/link-add.svg
        icons/link.svg icons/menu.svg icons/numbered-list.svg icons/paragraph.svg icons/quote.svg
        icons/search.svg icons/settings.svg icons/strikethrough.svg icons/table.svg
        icons/task.svg icons/text.svg icons/toolbar.svg
)

target_link_libraries(atlas-notepad-app PRIVATE ${APP_LIBS} atlas-ui atlas-uiplugin)

# Tests: the editing logic on a real TextEdit, offscreen.
#   ctest --test-dir <build> --output-on-failure
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # Plain add_executable: Qt's finalizer would scan for QML imports it doesn't need.
    add_executable(editor_test tests/editor_test.cpp
        cpp/markdown.h cpp/highlighter.cpp cpp/markdowneditor.cpp cpp/decorations.cpp)
    target_include_directories(editor_test PRIVATE cpp)
    target_link_libraries(editor_test PRIVATE atlas_notepad Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
    add_test(NAME editor_test COMMAND editor_test)
    set_tests_properties(editor_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Documents, tabs, the session and settings, with XDG dirs in a temp dir.
    add_executable(app_test tests/app_test.cpp
        cpp/markdown.h cpp/highlighter.cpp cpp/markdowneditor.cpp cpp/decorations.cpp
        ${APP_SOURCES})
    target_include_directories(app_test PRIVATE cpp)
    target_link_libraries(app_test PRIVATE ${APP_LIBS} Qt6::Test)
    add_test(NAME app_test COMMAND app_test)
    set_tests_properties(app_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

install(TARGETS atlas-notepad-app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/net.eterneon.atlas.notepad.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/net.eterneon.atlas.notepad.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/net.eterneon.atlas.notepad.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Drawn again for 16 px, where the 128 px lines would blur.
install(FILES data/net.eterneon.atlas.notepad-16.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/16x16/apps
        RENAME net.eterneon.atlas.notepad.svg)
