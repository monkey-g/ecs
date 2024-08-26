cmake_minimum_required (VERSION 3.15)

add_executable (static_example "static_example.cpp")
target_link_libraries(static_example ecs)
