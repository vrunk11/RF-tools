# What the windows share: the colour themes and widget styles with their menus, the editor of the custom theme, and the small
# arrow that reduces a panel (the look of ld-analyse and ld-stacker-gui).
add_library(rfx-gui-common STATIC
    appearancemenus.cpp
    collapse.cpp
    theme.cpp
    themeeditordialog.cpp)
target_include_directories(rfx-gui-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rfx-gui-common PUBLIC Qt6::Widgets Qt6::Core)
