# The Android build of the engine.
#
# The sources are the engine's own, from the branch this one is merged onto;
# nothing is copied and nothing is written twice. Two files are left out because
# they belong to the desktop runner and to nothing else: src/main.c is the
# command line, and src/png.c writes the --shot screenshot, which is how a
# container with no display checks a frame. jni.c beside this file takes their
# place: it steps a session and hands Java the pixels the engine composed.
#
# What is left needs no library at all beyond the C runtime: the archive, image
# and Ogg codecs are the engine's own, and the two vendored files in
# third_party/ compile inside the src file that uses them - stb_truetype.h
# inside src/font.c and stb_vorbis.c inside src/audio.c - so this glob never has
# to name a vendored file.

cmake_minimum_required(VERSION 3.22)
project(cmvs C)

set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party)

file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS ${ENGINE_DIR}/*.c)
list(REMOVE_ITEM ENGINE_SOURCES ${ENGINE_DIR}/main.c ${ENGINE_DIR}/png.c)
if(NOT ENGINE_SOURCES)
    message(FATAL_ERROR
        "No engine sources at ${ENGINE_DIR}. This branch is the wrapper alone; "
        "build a release branch, which merges it onto the engine.")
endif()

add_library(cmvs SHARED ${ENGINE_SOURCES} jni.c)
target_include_directories(cmvs PRIVATE ${ENGINE_DIR} ${VENDOR_DIR})
target_compile_options(cmvs PRIVATE -std=c11 -Wall -Wextra -Wno-unused-parameter -O2)
target_link_libraries(cmvs PRIVATE log m)
