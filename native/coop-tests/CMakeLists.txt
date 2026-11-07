# Online co-op's session on its own, without the game (no disc needed): the self-test and the connection probe
cmake_minimum_required(VERSION 3.20)
project(twinsanity_coop_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(COOP_SOURCES ${ROOT}/src/platform/native/coop/coopnet.cpp ${ROOT}/src/platform/native/coop/coopsession.cpp)

foreach(tool coop_selftest coopprobe)
    if(tool STREQUAL coop_selftest)
        add_executable(${tool} selftest.cpp ${COOP_SOURCES})
    else()
        add_executable(${tool} probe.cpp ${COOP_SOURCES})
    endif()
    target_include_directories(${tool} PRIVATE ${ROOT}/src/platform/native)
    # As the game is built: no exceptions, no RTTI
    target_compile_options(${tool} PRIVATE -fno-exceptions -fno-rtti -Wall)
    if(WIN32)
        target_link_libraries(${tool} PRIVATE ws2_32)
        target_link_options(${tool} PRIVATE -static)
    endif()
endforeach()
