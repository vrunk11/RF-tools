add_executable(rfx-library-tests library_tests.cpp)
target_link_libraries(rfx-library-tests PRIVATE rfx-library Qt6::Core)

# Every test carries exactly one label (unit or functional).
add_test(NAME rfx-library-tests COMMAND rfx-library-tests)
set_tests_properties(rfx-library-tests PROPERTIES LABELS unit)

add_executable(rf-requantize-tests requantize_tests.cpp)
target_link_libraries(rf-requantize-tests PRIVATE rfx-library Qt6::Core)
add_test(NAME rf-requantize-tests COMMAND rf-requantize-tests)
set_tests_properties(rf-requantize-tests PROPERTIES LABELS unit)

add_executable(rf-analyse-tests analysis_tests.cpp)
target_link_libraries(rf-analyse-tests PRIVATE rfx-library Qt6::Core)
add_test(NAME rf-analyse-tests COMMAND rf-analyse-tests)
set_tests_properties(rf-analyse-tests PROPERTIES LABELS unit)

# The window of rf-analyse, driven on an offscreen screen (needs Qt Widgets and Qt Test).
if(TARGET rf-analyse-gui AND TARGET Qt6::Test)
    add_executable(rf-analyse-gui-tests analyse_gui_tests.cpp)
    target_link_libraries(rf-analyse-gui-tests PRIVATE rf-analyse-gui Qt6::Widgets Qt6::Test)
    add_test(NAME rf-analyse-gui-tests COMMAND rf-analyse-gui-tests)
    set_tests_properties(rf-analyse-gui-tests PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(rfx-roundtrip-tests roundtrip_tests.cpp)
target_link_libraries(rfx-roundtrip-tests PRIVATE rfx-library Qt6::Core)
target_compile_definitions(rfx-roundtrip-tests PRIVATE RFX_PROGRAM="$<TARGET_FILE:rfx>")
add_dependencies(rfx-roundtrip-tests rfx)
add_test(NAME rfx-roundtrip-tests COMMAND rfx-roundtrip-tests)
set_tests_properties(rfx-roundtrip-tests PROPERTIES LABELS unit)

# The window of rf-packer, and the work it has done by the program itself (needs Qt Widgets and Qt Test).
if(TARGET rf-packer-gui AND TARGET Qt6::Test)
    add_executable(rf-packer-tests packer_tests.cpp)
    target_link_libraries(rf-packer-tests PRIVATE rf-packer-gui Qt6::Widgets Qt6::Test)
    target_compile_definitions(rf-packer-tests PRIVATE PACKER_PROGRAM="$<TARGET_FILE:rf-packer>")
    add_dependencies(rf-packer-tests rf-packer)
    add_test(NAME rf-packer-tests COMMAND rf-packer-tests)
    set_tests_properties(rf-packer-tests PROPERTIES LABELS unit ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
