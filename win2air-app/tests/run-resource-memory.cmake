cmake_minimum_required(VERSION 3.25)
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED CONFIGURATION)
  set(CONFIGURATION Debug)
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "CONFIGURATION must be Debug or Release.")
endif()
set(RUNTIME_ROOT "${APP_ROOT}/out/bin/${CONFIGURATION}")
if(NOT EXISTS "${RUNTIME_ROOT}/resources.pri" OR
   NOT EXISTS "${APP_ROOT}/out/obj/Win2Air/${CONFIGURATION}/Generated Files/winrt/Microsoft.Windows.ApplicationModel.Resources.h")
  message(FATAL_ERROR "Build the App first to prepare its resource runtime and WinRT headers.")
endif()
find_program(VSWHERE vswhere.exe PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer" REQUIRED NO_CACHE)
execute_process(COMMAND "${VSWHERE}" -latest -products * -version "[18.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  OUTPUT_VARIABLE VS_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT VS_ROOT)
  message(FATAL_ERROR "Visual Studio 2026 native C++ tools are required.")
endif()
execute_process(COMMAND "${VS_ROOT}/MSBuild/Current/Bin/MSBuild.exe"
  "${APP_ROOT}/tests/Win2Air.Resources.Tests.vcxproj" /nologo /verbosity:minimal "/p:Configuration=${CONFIGURATION}" /p:Platform=x64
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${APP_ROOT}/out/tests/${CONFIGURATION}/Win2Air.Resources.Tests.exe" "${RUNTIME_ROOT}"
  TIMEOUT 180 COMMAND_ERROR_IS_FATAL ANY)
