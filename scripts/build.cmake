cmake_minimum_required(VERSION 3.24)

get_filename_component(project_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED TAILOR_CONFIG)
    set(TAILOR_CONFIG Debug)
endif()
if(NOT TAILOR_CONFIG STREQUAL "Debug" AND NOT TAILOR_CONFIG STREQUAL "Release")
    message(FATAL_ERROR "TAILOR_CONFIG must be Debug or Release")
endif()
if(NOT DEFINED TAILOR_JOBS)
    set(TAILOR_JOBS 4)
endif()
if(NOT TAILOR_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "TAILOR_JOBS must be a positive integer")
endif()

string(TOLOWER "${TAILOR_CONFIG}" preset_name)
set(configure_args)
if(DEFINED TAILOR_QT_PREFIX AND NOT TAILOR_QT_PREFIX STREQUAL "")
    list(APPEND configure_args "-DCMAKE_PREFIX_PATH=${TAILOR_QT_PREFIX}")
endif()
if(DEFINED TAILOR_ARCH AND NOT TAILOR_ARCH STREQUAL "")
    if(NOT CMAKE_HOST_APPLE OR NOT TAILOR_ARCH MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "TAILOR_ARCH supports arm64 or x86_64 on macOS only")
    endif()
    string(APPEND preset_name "-macos-${TAILOR_ARCH}")
endif()
if(CMAKE_HOST_APPLE)
    if(DEFINED TAILOR_OSX_SYSROOT AND NOT TAILOR_OSX_SYSROOT STREQUAL "")
        set(osx_sdk "${TAILOR_OSX_SYSROOT}")
    elseif(NOT "$ENV{SDKROOT}" STREQUAL "")
        set(osx_sdk "$ENV{SDKROOT}")
    else()
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            RESULT_VARIABLE sdk_result OUTPUT_VARIABLE osx_sdk
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT sdk_result EQUAL 0)
            unset(osx_sdk)
        endif()
    endif()
    if(osx_sdk)
        list(APPEND configure_args "-DCMAKE_OSX_SYSROOT=${osx_sdk}")
    endif()
endif()

message(STATUS "Configuring preset: ${preset_name}")
execute_process(COMMAND "${CMAKE_COMMAND}" --preset "${preset_name}" ${configure_args}
    WORKING_DIRECTORY "${project_dir}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build --preset "${preset_name}" --parallel "${TAILOR_JOBS}"
    WORKING_DIRECTORY "${project_dir}"
    COMMAND_ERROR_IS_FATAL ANY)

if(TAILOR_TEST)
    get_filename_component(cmake_real_path "${CMAKE_COMMAND}" REALPATH)
    get_filename_component(cmake_bin_dir "${cmake_real_path}" DIRECTORY)
    find_program(ctest_executable NAMES ctest HINTS "${cmake_bin_dir}" REQUIRED)
    execute_process(COMMAND "${ctest_executable}" --preset "${preset_name}"
        WORKING_DIRECTORY "${project_dir}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

if(TAILOR_INSTALL)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build --preset "${preset_name}" --target install --parallel "${TAILOR_JOBS}"
        WORKING_DIRECTORY "${project_dir}"
        COMMAND_ERROR_IS_FATAL ANY)
    message(STATUS "Installed to ${project_dir}/dist/${preset_name}")
endif()
