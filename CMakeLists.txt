cmake_minimum_required(VERSION 3.0)

project(osu-intercept)

add_executable(osu-intercept osu-intercept.c)
target_compile_options(osu-intercept PRIVATE -Wall -Wextra)

install(TARGETS osu-intercept RUNTIME DESTINATION bin)

