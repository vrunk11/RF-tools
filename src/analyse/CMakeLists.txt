# The window as a library, so that the tests can drive it; the program is main.cpp.
add_library(rf-analyse-gui STATIC
    capturedialog.cpp
    readout.cpp
    deviationview.cpp
    mainwindow.cpp
    metadatapanel.cpp
    session.cpp
    spectrumview.cpp
    statspanel.cpp
    timebase.cpp
    trackeditdialog.cpp
    tracklist.cpp
    waveformview.cpp
    rf-analyse-resources.qrc)
set_target_properties(rf-analyse-gui PROPERTIES AUTORCC ON)
target_include_directories(rf-analyse-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rf-analyse-gui PUBLIC rfx-library rfx-gui-common Qt6::Widgets Qt6::Core)

if(WIN32)
    # The icon of the program in Windows (the window's own comes from the resources).
    add_executable(rf-analyse main.cpp appicon.rc)
else()
    add_executable(rf-analyse main.cpp)
endif()
target_compile_definitions(rf-analyse PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_BRANCH="${APP_BRANCH}"
    APP_COMMIT="${APP_COMMIT}")
target_link_libraries(rf-analyse PRIVATE rf-analyse-gui)

# A program with a window: no console on Windows.
if(WIN32)
    set_target_properties(rf-analyse PROPERTIES WIN32_EXECUTABLE ON)
endif()

install(TARGETS rf-analyse RUNTIME DESTINATION bin)

# The icon and the menu entry on Linux.
if(UNIX AND NOT APPLE)
    foreach(size 16 32 64 128 256)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Graphics/${size}-rf-analyse.png
            DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME rf-analyse.png)
    endforeach()
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/install/rf-analyse.desktop DESTINATION share/applications)
endif()
