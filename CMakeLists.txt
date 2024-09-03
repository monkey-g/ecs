cmake_minimum_required (VERSION 3.28)

include(FetchContent)

project (ecs CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
set(ECS_STANDALONE_PROJECT OFF)


# Set up some user-changeable options
option(ECS_COMPILE_AS_MODULE "Compile 'ecs' to a module" OFF)
option(ECS_ENABLE_CONTRACTS "Use contracts internally to verify state" ON)
option(ECS_ENABLE_CONTRACTS_AUDIT "Enable expensive contracts. Should only be used for debugging" OFF)

# Set up module scanning
set(CMAKE_CXX_SCAN_FOR_MODULES ${ECS_COMPILE_AS_MODULE})

# Set up the project
if (ECS_COMPILE_AS_MODULE)
	# creates module library 'ecs'
	add_library(ecs)
	target_compile_definitions(ecs PUBLIC ECS_USE_MODULES)
	target_compile_definitions(ecs PRIVATE ECS_EXPORT=export)
	target_sources(ecs PUBLIC FILE_SET CXX_MODULES FILES "include/ecs/ecs.ixx")

	# Set project scope
	set(ECS_INTERNAL_SCOPE PUBLIC)
	set(ECS_INTERNAL_SCOPE_PRIV PRIVATE)
else()
	# creates header-only library 'ecs'
	add_library(ecs INTERFACE)
	target_compile_definitions(ecs INTERFACE ECS_EXPORT=)

	# Set project scope
	set(ECS_INTERNAL_SCOPE INTERFACE)
	set(ECS_INTERNAL_SCOPE_PRIV INTERFACE)
endif()


# Setup contract definitions
target_compile_definitions(ecs ${ECS_INTERNAL_SCOPE_PRIV}
	ECS_ENABLE_CONTRACTS=$<BOOL:${ECS_ENABLE_CONTRACTS}>
	ECS_ENABLE_CONTRACTS_AUDIT=$<IF:$<AND:$<BOOL:${ECS_ENABLE_CONTRACTS}>,$<BOOL:${ECS_ENABLE_CONTRACTS_AUDIT}>>,1,0>
)

# determine whether this is a standalone project or included by other projects
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(ECS_STANDALONE_PROJECT ON)
endif ()

# Set compiler-specific flags
if (MSVC)
	target_compile_options(ecs ${ECS_INTERNAL_SCOPE}
        # enable lots of warnings
        /W4

		# allow `import std.core`
		#/experimental:module
		#/EHsc

        # -- Dump compilation info ---
        #/Bt                     # total time spent in frontend (d1) and backend (d2)
        #/d1templateStats        # show info regarding template use
        #/d1reportTimeSummary    # show frontend compile time summary
        #/d1reportTime           # show (very) detailed frontend compile time info
        #/d2cgsummary            # show backend compile time summary

        # -- Enable conversion warnings --
        /w44191     # 'operator': unsafe conversion from 'type_of_expression' to 'type_required'.
        /w44242     # 'identifier': conversion from 'type1' to 'type2', possible loss of data.
        /w44254     # 'operator': conversion from 'type1' to 'type2', possible loss of data.
        /w44800     # Implicit conversion from 'type' to bool. Possible information loss.
        /w44826     # Conversion from 'type1' to 'type2' is sign-extended. This may cause unexpected runtime behavior.
        /w45219     # implicit conversion from 'type-1' to 'type-2', possible loss of data.
        /w44388     # signed/unsigned mismatch
        #/w44365     # 'action': conversion from 'type_1' to 'type_2', signed/unsigned mismatch.

		# Shows the layout of the struct name 'chunk'
		#/d1reportSingleClassLayoutchunk

        # Enable address sanitizer
        #/fsanitize=address
	)

	# enable profiling for 'RelWithDebInfo' builds
	if (CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
		target_compile_options(ecs ${ECS_INTERNAL_SCOPE} /Zi)
		target_link_options(ecs ${ECS_INTERNAL_SCOPE} /PROFILE)
	endif()
else()
    # lots of warnings
    target_compile_options(ecs ${ECS_INTERNAL_SCOPE}
        #-std=c++2a
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
		-Wno-deprecated-declarations # silence deprecations in libstdc++, which breaks compilation
        #-Wsign-conversion
        #-ftime-trace  # produce json flamegraph files. open with chrome://tracing or edge://tracing
	)
    target_link_options(ecs ${ECS_INTERNAL_SCOPE} -pthread)
endif()


# Project headers.
# add include folders to the library and targets that consume it
# the SYSTEM keyword suppresses warnings for users of the library
if (ECS_COMPILE_AS_MODULE)
	if(ECS_STANDALONE_PROJECT)
		target_include_directories(ecs SYSTEM ${ECS_INTERNAL_SCOPE}
			$<BUILD_INTERFACE:
				${CMAKE_CURRENT_SOURCE_DIR}/include
			>
		)
	endif()
else()
	if(ECS_STANDALONE_PROJECT)
		target_include_directories(ecs INTERFACE
			$<BUILD_INTERFACE:
				${CMAKE_CURRENT_SOURCE_DIR}/include
			>
		)
	else()
		target_include_directories(ecs SYSTEM INTERFACE
			$<BUILD_INTERFACE:
				${CMAKE_CURRENT_SOURCE_DIR}/include
			>
		)
	endif()
endif()

# add tls library
#add_subdirectory ("tls")
FetchContent_Declare(tls
  GIT_REPOSITORY "https://github.com/kgorking/tls.git"
  GIT_TAG "origin/master"
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/tls"
)
FetchContent_MakeAvailable(tls)
target_link_libraries(ecs ${ECS_INTERNAL_SCOPE} tls)

if (ECS_STANDALONE_PROJECT)
    # Benchmark
    add_subdirectory ("benchmark")

    # Examples
    add_subdirectory ("examples/entt_example")
    add_subdirectory ("examples/example")
    add_subdirectory ("examples/static_example")
    add_subdirectory ("examples/variant")
    add_subdirectory ("examples/filtering")
    add_subdirectory ("examples/interval")
    add_subdirectory ("examples/finite_state_machine")
    add_subdirectory ("examples/global_component")
    add_subdirectory ("examples/hierarchy")
    add_subdirectory ("examples/mandelbrot")
    add_subdirectory ("examples/parallelism")
    add_subdirectory ("examples/concurrency")
    add_subdirectory ("examples/sorting")
    add_subdirectory ("examples/tagged_components")
    add_subdirectory ("examples/custom_contract_violation_handler")

    # Unittests
    enable_testing()
    add_subdirectory ("unittest")
endif ()
