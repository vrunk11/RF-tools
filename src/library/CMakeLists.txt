add_library(rfx-library STATIC
    alignment.cpp
    archive.cpp
    capture.cpp
    convert.cpp
    edit.cpp
    ffmpeg.cpp
    flacmode.cpp
    ebml.cpp
    lagtable.cpp
    metadata.cpp
    order.cpp
    packer.cpp
    posfile.cpp
    predictor.cpp
    refwindow.cpp
    residualmodel.cpp
    sampleformat.cpp
    segment.cpp
    yaml.cpp)

target_include_directories(rfx-library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rfx-library PUBLIC Qt6::Core)

find_package(Threads REQUIRED)
target_link_libraries(rfx-library PUBLIC Threads::Threads)
