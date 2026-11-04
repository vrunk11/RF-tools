# The window as a library, so that the tests can drive it; the program is main.cpp.
add_library(rf-analyse-gui STATIC
    appearancemenus.cpp
    capturedialog.cpp
    collapse.cpp
    readout.cpp
    deviationview.cpp
    mainwindow.cpp
    metadatapanel.cpp
    session.cpp
    spectrumview.cpp
    statspanel.cpp
    tracklist.cpp
    theme.cpp
    themeeditordialog.cpp
    waveformview.cpp
    rf-analyse-resources.qrc)
set_target_properties(rf-analyse-gui PROPERTIES AUTORCC ON)
target_include_directories(rf-analyse-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rf-analyse-gui PUBLIC rfx-library Qt6::Widgets Qt6::Core)

add_executable(rf-analyse main.cpp)
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
