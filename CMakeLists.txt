cmake_minimum_required(VERSION 3.20)

project(TransportOffloadScheduler
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime that decides where transport-path work executes"
  HOMEPAGE_URL "https://github.com/pngen/Transport-Offload-Scheduler"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TOS_IS_TOP_LEVEL ON)
else()
  set(TOS_IS_TOP_LEVEL OFF)
endif()

option(TOS_BUILD_TESTS "Build the Transport Offload Scheduler test suite" ${TOS_IS_TOP_LEVEL})
option(TOS_BUILD_EXAMPLES "Build the Transport Offload Scheduler examples" ${TOS_IS_TOP_LEVEL})
option(TOS_BUILD_BENCHMARKS "Build the Transport Offload Scheduler benchmarks" ${TOS_IS_TOP_LEVEL})
option(TOS_BUILD_APPS "Build the coordinator, worker and inspection tools" ${TOS_IS_TOP_LEVEL})
option(TOS_ENABLE_CUDA "Build the real CUDA accelerator backend when a toolkit is present" ON)
option(TOS_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(TOS_ENABLE_ASAN "Build the first-party core with AddressSanitizer" OFF)
option(TOS_ENABLE_LOGGING "Compile runtime logging support" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- sources ---------------------------------------------------------------
file(GLOB_RECURSE TOS_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/backends/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dist/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/persist/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp")
# nvcc sources and the bridge that stands in for them are added explicitly below, so
# that exactly one implementation of the CUDA bridge is ever linked.
list(FILTER TOS_CORE_SOURCES EXCLUDE REGEX "\.cu$")
list(FILTER TOS_CORE_SOURCES EXCLUDE REGEX "cuda_bridge_stub\.cpp$")

add_library(transport_offload_scheduler ${TOS_CORE_SOURCES})
add_library(SummonSoftwareLabs::TransportOffloadScheduler ALIAS transport_offload_scheduler)
add_library(tos::tos ALIAS transport_offload_scheduler)

target_include_directories(transport_offload_scheduler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(transport_offload_scheduler
  PUBLIC TOS_VERSION_STRING="${PROJECT_VERSION}"
  PRIVATE TOS_BUILDING_LIBRARY=1)

if(TOS_ENABLE_LOGGING)
  target_compile_definitions(transport_offload_scheduler PUBLIC TOS_ENABLE_LOGGING=1)
endif()

# ---- warnings --------------------------------------------------------------
if(MSVC)
  set(TOS_WARNING_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(TOS_WARNINGS_AS_ERRORS)
    list(APPEND TOS_WARNING_FLAGS /WX)
  endif()
  set(TOS_RELEASE_FLAGS /O2 /Gy /Gw)
  set(TOS_DEBUG_FLAGS /Od /RTC1)
else()
  set(TOS_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(TOS_WARNINGS_AS_ERRORS)
    list(APPEND TOS_WARNING_FLAGS -Werror)
  endif()
  set(TOS_RELEASE_FLAGS -O2)
  set(TOS_DEBUG_FLAGS -O0 -g)
endif()

target_compile_options(transport_offload_scheduler PRIVATE
  ${TOS_WARNING_FLAGS}
  $<$<CONFIG:Release>:${TOS_RELEASE_FLAGS}>
  $<$<CONFIG:RelWithDebInfo>:${TOS_RELEASE_FLAGS}>
  $<$<CONFIG:Debug>:${TOS_DEBUG_FLAGS}>)

if(TOS_ENABLE_ASAN)
  if(MSVC)
    # /Zi is required for meaningful ASan reports; /wd5072 silences the advisory that
    # fires when a translation unit is instrumented without debug information, which
    # would otherwise fail the build under /WX.
    target_compile_options(transport_offload_scheduler PUBLIC /fsanitize=address /Zi /wd5072)
  else()
    target_compile_options(transport_offload_scheduler PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(transport_offload_scheduler PUBLIC -fsanitize=address)
  endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(transport_offload_scheduler PRIVATE Threads::Threads)
target_link_libraries(transport_offload_scheduler PUBLIC $<$<PLATFORM_ID:Windows>:ws2_32> $<$<PLATFORM_ID:Windows>:iphlpapi>)

set_target_properties(transport_offload_scheduler PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME "transport_offload_scheduler"
  EXPORT_NAME "TransportOffloadScheduler"
  DEBUG_POSTFIX "d")


# ---- optional real CUDA accelerator backend --------------------------------
# The accelerator backend is real only when a CUDA toolkit is present. Without one the
# bridge falls back to a stub that reports UNSUPPORTED, so no build ever claims
# hardware it cannot use. The device translation unit is compiled in its own object
# library so that host-only warning flags are not forced through nvcc.
if(TOS_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
      set(CMAKE_CUDA_ARCHITECTURES 75 120 CACHE STRING "CUDA architectures to generate code for")
    endif()
    enable_language(CUDA)
    message(STATUS "CUDA toolkit found: building the real accelerator backend (${CMAKE_CUDA_ARCHITECTURES})")
    add_library(tos_cuda_kernels OBJECT src/backends/cuda_kernels.cu)
    set_target_properties(tos_cuda_kernels PROPERTIES
      CUDA_STANDARD 20
      CUDA_STANDARD_REQUIRED ON
      CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
      POSITION_INDEPENDENT_CODE ON)
    target_include_directories(tos_cuda_kernels PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(tos_cuda_kernels PRIVATE --expt-relaxed-constexpr)
    if(MSVC)
      # /wd4211 silences a warning nvcc's own generated stub file raises, which would
      # otherwise fail under /WX. First-party CUDA code stays clean at /W4.
      target_compile_options(tos_cuda_kernels PRIVATE "-Xcompiler=/W4" "-Xcompiler=/wd4211")
    else()
      target_compile_options(tos_cuda_kernels PRIVATE -Xcompiler=-Wall)
    endif()
    target_sources(transport_offload_scheduler PRIVATE $<TARGET_OBJECTS:tos_cuda_kernels>)
    target_compile_definitions(transport_offload_scheduler PRIVATE TOS_HAS_CUDA=1)
  else()
    message(STATUS "no CUDA toolkit: the accelerator backend will report UNSUPPORTED")
    target_sources(transport_offload_scheduler PRIVATE src/backends/cuda_bridge_stub.cpp)
  endif()
else()
  message(STATUS "TOS_ENABLE_CUDA=OFF: the accelerator backend will report UNSUPPORTED")
  target_sources(transport_offload_scheduler PRIVATE src/backends/cuda_bridge_stub.cpp)
endif()

# ---- helpers ---------------------------------------------------------------
function(tos_add_tool target)
  foreach(source ${ARGN})
    if(NOT EXISTS "${source}")
      message(STATUS "skipping ${target}: ${source} not present")
      return()
    endif()
  endforeach()
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE transport_offload_scheduler)
  target_compile_options(${target} PRIVATE ${TOS_WARNING_FLAGS})
  target_compile_options(${target} PRIVATE
    $<$<CONFIG:Release>:${TOS_RELEASE_FLAGS}>
    $<$<CONFIG:Debug>:${TOS_DEBUG_FLAGS}>)
endfunction()

if(TOS_BUILD_APPS)
  tos_add_tool(tos_coordinator apps/tos_coordinator.cpp)
  tos_add_tool(tos_worker apps/tos_worker.cpp)
  tos_add_tool(tos_inspect apps/tos_inspect.cpp)
endif()

if(TOS_BUILD_EXAMPLES)
  file(GLOB TOS_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
  foreach(example_source ${TOS_EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_source} NAME_WE)
    tos_add_tool(example_${example_name} ${example_source})
  endforeach()
endif()

if(TOS_BUILD_BENCHMARKS)
  tos_add_tool(tos_benchmark benchmarks/tos_benchmark.cpp)
endif()

if(TOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- installation ----------------------------------------------------------
install(TARGETS transport_offload_scheduler
  EXPORT TransportOffloadSchedulerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT TransportOffloadSchedulerTargets
  FILE TransportOffloadSchedulerTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TransportOffloadScheduler)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TransportOffloadSchedulerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TransportOffloadSchedulerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TransportOffloadScheduler)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TransportOffloadSchedulerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TransportOffloadSchedulerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TransportOffloadSchedulerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TransportOffloadScheduler)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/TransportOffloadScheduler)