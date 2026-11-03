add_executable(rfx-library-tests library_tests.cpp)
target_link_libraries(rfx-library-tests PRIVATE rfx-library Qt6::Core)

# Every test carries exactly one label (unit or functional).
add_test(NAME rfx-library-tests COMMAND rfx-library-tests)
set_tests_properties(rfx-library-tests PROPERTIES LABELS unit)

add_executable(rfx-roundtrip-tests roundtrip_tests.cpp)
target_link_libraries(rfx-roundtrip-tests PRIVATE rfx-library Qt6::Core)
add_test(NAME rfx-roundtrip-tests COMMAND rfx-roundtrip-tests)
set_tests_properties(rfx-roundtrip-tests PROPERTIES LABELS unit)
