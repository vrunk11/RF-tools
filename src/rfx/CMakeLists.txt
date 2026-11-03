add_executable(rfx main.cpp)

target_compile_definitions(rfx PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_BRANCH="${APP_BRANCH}"
    APP_COMMIT="${APP_COMMIT}")
target_link_libraries(rfx PRIVATE rfx-library Qt6::Core)

install(TARGETS rfx RUNTIME DESTINATION bin)
