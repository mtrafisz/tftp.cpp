cmake_minimum_required (VERSION 3.5)
project (tftpc)
set (CMAKE_CXX_STANDARD 17)

file (GLOB LIB_SOURCES "src/*.cpp")
file (GLOB TEST_SOURCES "test/*.cpp")

if (WIN32)
	list (APPEND libs "ws2_32")
endif (WIN32)

add_library (tftpc STATIC ${LIB_SOURCES})
target_link_libraries (tftpc ${libs})
add_library (dyntftpc SHARED ${LIB_SOURCES})
target_link_libraries (dyntftpc ${libs})

foreach (TEST_SOURCE ${TEST_SOURCES})
	get_filename_component (TEST_NAME ${TEST_SOURCE} NAME_WE)
	add_executable (${TEST_NAME} ${TEST_SOURCE})
	target_link_libraries (${TEST_NAME} tftpc)
	add_test (${TEST_NAME} ${TEST_NAME})
endforeach (TEST_SOURCE ${TEST_SOURCES})

set_target_properties (tftpc PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
