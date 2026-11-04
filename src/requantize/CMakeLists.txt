add_executable(rf-requantize main.cpp)

target_compile_definitions(rf-requantize PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_BRANCH="${APP_BRANCH}"
    APP_COMMIT="${APP_COMMIT}")
target_link_libraries(rf-requantize PRIVATE rfx-library Qt6::Core)

install(TARGETS rf-requantize RUNTIME DESTINATION bin)
