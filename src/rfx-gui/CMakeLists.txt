add_executable(rfx-gui WIN32 MACOSX_BUNDLE main.cpp)

target_compile_definitions(rfx-gui PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_BRANCH="${APP_BRANCH}"
    APP_COMMIT="${APP_COMMIT}")
target_link_libraries(rfx-gui PRIVATE Qt6::Core Qt6::Widgets)

set_target_properties(rfx-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "rfx-gui"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.rf-tools.rfx-gui"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

install(TARGETS rfx-gui
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE)
    install(FILES rfx-gui.desktop DESTINATION share/applications)
    install(FILES rfx-gui.svg DESTINATION share/icons/hicolor/scalable/apps)
endif()

# Starts the program, builds its window and leaves, with no display: what the
# CI uses to check a build or a package (--help would open a message box on
# Windows). Replace with real tests as the program grows; every test carries
# exactly one label (unit or functional).
add_test(NAME rfx-gui-self-test COMMAND rfx-gui --self-test)
set_tests_properties(rfx-gui-self-test PROPERTIES
    LABELS unit
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
