cmake_minimum_required(VERSION 3.25)
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED CONFIGURATION)
  set(CONFIGURATION Debug)
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "CONFIGURATION must be Debug or Release.")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCONFIGURATION=${CONFIGURATION}"
  -P "${APP_ROOT}/scripts/build-app.cmake"
  TIMEOUT 900 COMMAND_ERROR_IS_FATAL ANY)
find_program(VSWHERE vswhere.exe PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer" REQUIRED NO_CACHE)
execute_process(COMMAND "${VSWHERE}" -latest -products * -version "[18.0,19.0)"
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  OUTPUT_VARIABLE VS_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT VS_ROOT)
  message(FATAL_ERROR "Visual Studio 2026 native C++ tools are required.")
endif()
execute_process(COMMAND "${VS_ROOT}/MSBuild/Current/Bin/MSBuild.exe"
  "${APP_ROOT}/tests/Win2Air.ConnectedMemory.Tests.vcxproj"
  /nologo /verbosity:minimal "/p:Configuration=${CONFIGURATION}" /p:Platform=x64
  TIMEOUT 300 COMMAND_ERROR_IS_FATAL ANY)
set(REPORT "${APP_ROOT}/out/connected-memory-${CONFIGURATION}.json")
file(REMOVE "${REPORT}")
execute_process(COMMAND "${APP_ROOT}/out/bin/${CONFIGURATION}/Win2Air.ConnectedMemory.Tests.exe" "${REPORT}"
  TIMEOUT 300 COMMAND_ERROR_IS_FATAL ANY)
file(READ "${REPORT}" RESULT)
string(JSON STATUS GET "${RESULT}" status)
if(NOT STATUS STREQUAL "passed")
  message(FATAL_ERROR "Connected memory test failed: ${RESULT}")
endif()
message(STATUS "Connected capture, metrics and page switching regression passed: ${REPORT}")
