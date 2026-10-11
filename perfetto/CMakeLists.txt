cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(perflens_perfetto VERSION 0.1.0 LANGUAGES C CXX)
  include(CTest)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(BUILD_SHARED_LIBS "Build the instrumentation library as a shared library" OFF)
option(PERFLENS_BUILD_EXAMPLES "Build instrumentation examples" ON)
set(PERFLENS_PERFETTO_SDK_DIR "" CACHE PATH "Offline directory containing perfetto.h and perfetto.cc")
if(NOT PERFLENS_PERFETTO_SDK_DIR)
  include(FetchContent)
  FetchContent_Declare(perflens_sdk
    URL https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip
    URL_HASH SHA256=e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09)
  FetchContent_MakeAvailable(perflens_sdk)
  set(PERFLENS_PERFETTO_SDK_DIR "${perflens_sdk_SOURCE_DIR}")
endif()
if(NOT EXISTS "${PERFLENS_PERFETTO_SDK_DIR}/perfetto.cc")
  message(FATAL_ERROR "PERFLENS_PERFETTO_SDK_DIR must contain the Perfetto v58.2 C++ SDK")
endif()

# Include the SDK objects in our archive, so consumers need only one library.
add_library(perflens_sdk_objects OBJECT "${PERFLENS_PERFETTO_SDK_DIR}/perfetto.cc")
target_compile_features(perflens_sdk_objects PRIVATE cxx_std_17)
set_target_properties(perflens_sdk_objects PROPERTIES POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

add_library(perflens_trace src/trace.cpp $<TARGET_OBJECTS:perflens_sdk_objects>)
add_library(perflens::trace ALIAS perflens_trace)
target_compile_features(perflens_trace PUBLIC cxx_std_17)
target_include_directories(perflens_trace PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${PERFLENS_PERFETTO_SDK_DIR}")
target_link_libraries(perflens_trace PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(perflens_trace PROPERTIES EXPORT_NAME trace VERSION 0.1.0
  SOVERSION 0 CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(perflens_trace PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
  set_property(TARGET perflens_trace APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
elseif(BUILD_SHARED_LIBS AND APPLE)
  target_link_options(perflens_trace PRIVATE
    "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/exports.macos")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/configs/cpu.pbtxt.in")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/configs/cpu.pbtxt.in" CPU_CONFIG_TEMPLATE)
configure_file(src/cli/config_template.hpp.in generated/config_template.hpp @ONLY)
add_library(perflens_cli_config STATIC src/cli/config.cpp)
target_compile_features(perflens_cli_config PUBLIC cxx_std_17)
target_include_directories(perflens_cli_config PUBLIC src/cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(perflens-trace src/cli/main.cpp)
target_link_libraries(perflens-trace PRIVATE perflens_cli_config Threads::Threads)
target_compile_options(perflens_trace PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(perflens_cli_config PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(perflens-trace PRIVATE -Wall -Wextra -Wpedantic)

if(PERFLENS_BUILD_EXAMPLES)
  add_executable(perflens-demo examples/demo.cpp)
  target_link_libraries(perflens-demo PRIVATE perflens::trace Threads::Threads)
endif()
if(BUILD_TESTING)
  add_executable(perflens-config-test tests/config_test.cpp)
  target_link_libraries(perflens-config-test PRIVATE perflens_cli_config)
  add_test(NAME perfetto.config COMMAND perflens-config-test)
  add_executable(perflens-disabled-test tests/disabled_test.cpp)
  target_include_directories(perflens-disabled-test PRIVATE include)
  target_compile_definitions(perflens-disabled-test PRIVATE PERFLENS_TRACE_DISABLED)
  target_compile_features(perflens-disabled-test PRIVATE cxx_std_17)
  add_test(NAME perfetto.disabled COMMAND perflens-disabled-test)
  add_executable(perflens-c-api-test tests/c_api_test.c)
  target_link_libraries(perflens-c-api-test PRIVATE perflens::trace)
  set_target_properties(perflens-c-api-test PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME perfetto.c_api COMMAND perflens-c-api-test)
  add_test(NAME perfetto.cli_help COMMAND perflens-trace --help)
endif()

install(TARGETS perflens_trace EXPORT perflensTraceTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS perflens-trace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/perfetto)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" DESTINATION ${CMAKE_INSTALL_DOCDIR}/perfetto)
install(EXPORT perflensTraceTargets NAMESPACE perflens:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/perflensTrace)
configure_package_config_file(cmake/perflensTraceConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/perflensTraceConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/perflensTrace)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/perflensTraceConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/perflensTrace)
