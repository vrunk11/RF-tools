add_library(rfx-library STATIC
    alignment.cpp
    analysis.cpp
    archive.cpp
    capture.cpp
    convert.cpp
    diagnostics.cpp
    edit.cpp
    ffmpeg.cpp
    flacmode.cpp
    gpupipeline.cpp
    ebml.cpp
    lagtable.cpp
    metadata.cpp
    opencl.cpp
    gpupredictor.cpp
    order.cpp
    packer.cpp
    posfile.cpp
    predictor.cpp
    refwindow.cpp
    requantize.cpp
    requantizejob.cpp
    requantizer.cpp
    rfsync.cpp
    residualmodel.cpp
    sampleformat.cpp
    segment.cpp
    sha256.cpp
    trackreader.cpp
    verify.cpp
    yaml.cpp)

target_include_directories(rfx-library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rfx-library PUBLIC Qt6::Core)

# Files of many gigabytes: 64-bit offsets with the C library on every system.
target_compile_definitions(rfx-library PUBLIC _FILE_OFFSET_BITS=64)

find_package(Threads REQUIRED)
target_link_libraries(rfx-library PUBLIC Threads::Threads)

# The OpenCL kernel is embedded as bytes (a string literal of that size is more
# than MSVC accepts).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/kernel.cl RFX_KERNEL_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," RFX_KERNEL_BYTES "${RFX_KERNEL_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/kernel_source.inc "${RFX_KERNEL_BYTES}0x00\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kernel.cl)
target_include_directories(rfx-library PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
