cmake_minimum_required(VERSION 3.24)
project(atlas-notepad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX /usr CACHE PATH "" FORCE)
endif()
include(GNUInstallDirs)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)
find_package(Corrosion REQUIRED)

set(WORKSPACE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# Rust: the Markdown reader (and, later, files and the session).
corrosion_import_crate(MANIFEST_PATH ${WORKSPACE_DIR}/Cargo.toml
    CRATES atlas-notepad LOCKED)

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
    NO_IMPORT_SCAN
    QML_FILES
        qml/Main.qml qml/ToolIcon.qml
    SOURCES
        cpp/markdown.h cpp/highlighter.cpp cpp/markdowneditor.cpp cpp/decorations.cpp
    RESOURCES
        icons/bold.svg icons/bullet-list.svg icons/checklist.svg icons/chevron-down.svg
        icons/chevron-right.svg icons/chevron-up.svg icons/close.svg icons/code.svg
        icons/copy.svg icons/divider.svg icons/find-replace.svg icons/heading1.svg
        icons/heading2.svg icons/image.svg icons/italic.svg icons/link-add.svg
        icons/link.svg icons/menu.svg icons/paragraph.svg icons/quote.svg
        icons/search.svg icons/settings.svg icons/strikethrough.svg icons/table.svg
        icons/task.svg icons/text.svg icons/toolbar.svg
)

target_link_libraries(atlas-notepad-app PRIVATE
    atlas_notepad
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)

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
endif()

install(TARGETS atlas-notepad-app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/net.eterneon.atlas.notepad.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
