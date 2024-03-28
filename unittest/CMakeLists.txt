cmake_minimum_required (VERSION 3.15)

set(ECS_ENABLE_CONTRACTS ON)

# Download Catch2
FetchContent_Declare(catch2
  GIT_REPOSITORY "https://github.com/catchorg/catch2.git"
  GIT_TAG "v3.5.3"
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/catch2"
)
FetchContent_MakeAvailable(catch2)


macro(make_test)
	add_executable (${ARGV0} ${ARGV1})
	target_link_libraries(${ARGV0} ecs Catch2::Catch2WithMain)
	add_test(${ARGV0} ${ARGV0})

if (MSVC)
	#target_compile_options(${ARGV0} PRIVATE  /bigobj) # thanks hierarchy.cpp
	#target_compile_options(${ARGV0} PRIVATE /RTCs /fsanitize=address)
else()
	target_compile_options(${ARGV0} PRIVATE -fsanitize=undefined,address)
	target_link_options(${ARGV0} PRIVATE -fsanitize=undefined,address)
endif()
endmacro()

make_test (test_component_removal "component_removal.cpp")
make_test (test_component_pool "component_pool.cpp")
make_test (test_entity_range "entity_range.cpp")
make_test (test_runtime "runtime.cpp")
make_test (test_systems "system.cpp")
make_test (test_transient_components "transient_components.cpp")
make_test (test_global_component "global_component.cpp")
make_test (test_sorting "sorting.cpp")
make_test (test_filtering "filtering.cpp")
make_test (test_scheduler "scheduler.cpp")
make_test (test_system_options "system_options.cpp")
make_test (test_interval "interval.cpp")
make_test (test_hierarchy "hierarchy.cpp")
make_test (test_tagged_pointer "tagged_pointer.cpp")
make_test (test_type_list "type_list_tests.cpp")
make_test (test_variant "variant.cpp")
make_test (test_listarray "listarray.cpp")
make_test (test_array_scatter_allocator "scatter_allocator.cpp")
