cmake_minimum_required(VERSION 3.21)

project(
	ScriptLoadMonitor
	VERSION 1.0.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

file(GLOB_RECURSE headers CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "source/*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${headers} ${sources})

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(spdlog REQUIRED CONFIG)
find_package(directxtk CONFIG REQUIRED)  # CommonLibSSE-NG links Microsoft::DirectXTK

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})
add_library("${PROJECT_NAME}::${PROJECT_NAME}" ALIAS "${PROJECT_NAME}")

if (MSVC)
	# Debug symbols (rule 43) and no build-machine paths in the DLL (rule 45).
	target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
	target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
	file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" PROJECT_NATIVE_DIR)
	target_compile_options(${PROJECT_NAME} PRIVATE "/d1trimfile:${PROJECT_NATIVE_DIR}")
	target_link_options(${PROJECT_NAME} PRIVATE /PDBALTPATH:%_PDB%)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibSSE::CommonLibSSE
		spdlog::spdlog_header_only
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
