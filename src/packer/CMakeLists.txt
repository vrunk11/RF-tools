# The window as a library, so that the tests can drive it; the program is main.cpp. The program is also its own worker: the window
# runs it again with --job to do the writing.
add_library(rf-packer-gui STATIC
    document.cpp
    job.cpp
    mainwindow.cpp
    processdialog.cpp
    trackrow.cpp)
target_include_directories(rf-packer-gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rf-packer-gui PUBLIC rfx-library rfx-gui-common Qt6::Widgets Qt6::Core)

add_executable(rf-packer main.cpp)
target_compile_definitions(rf-packer PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_BRANCH="${APP_BRANCH}"
    APP_COMMIT="${APP_COMMIT}")
target_link_libraries(rf-packer PRIVATE rf-packer-gui)

# A program with a window: no console on Windows.
if(WIN32)
    set_target_properties(rf-packer PROPERTIES WIN32_EXECUTABLE ON)
endif()

install(TARGETS rf-packer RUNTIME DESTINATION bin)
