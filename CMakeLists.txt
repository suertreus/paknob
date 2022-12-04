cmake_minimum_required(VERSION 3.10)

project(patool)

set(CMAKE_CXX_STANDARD 17)

find_package(absl REQUIRED)
find_package(PulseAudio REQUIRED)

add_executable(patool patool.cc)

target_link_libraries(patool ${PULSEAUDIO_LIBRARY} absl::any_invocable absl::str_format absl::strings absl::span)

install(TARGETS patool)
