cmake_minimum_required(VERSION 3.31)
project(synth_player)

set(CMAKE_CXX_STANDARD 20)

include_directories(include include/sinf)
link_directories(lib)

add_executable(synth_player main.cpp
        include/splayer/splayer.c
        include/sinf/serrno.c
        include/sfs/sfs.c
        include/solfege/solfege.c
        include/wav/wav.c
        include/freeverb/freeverb.c)

target_link_libraries(synth_player LINK_PUBLIC soundio)

set(CMAKE_CXX_FLAGS "-O0 -Wdouble-promotion -Werror -fsingle-precision-constant")