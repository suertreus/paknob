cmake_minimum_required(VERSION 3.10)

project(paknob)

set(CMAKE_CXX_STANDARD 17)

find_package(absl REQUIRED)
find_package(PulseAudio REQUIRED)

add_executable(paknob paknob.cc)

target_link_libraries(paknob ${PULSEAUDIO_LIBRARY} absl::any_invocable absl::str_format absl::strings absl::span)

install(TARGETS paknob)
