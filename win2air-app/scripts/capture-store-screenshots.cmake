cmake_minimum_required(VERSION 3.25)
get_filename_component(APP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED CONFIGURATION)
  set(CONFIGURATION Debug)
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "CONFIGURATION must be Debug or Release.")
endif()
execute_process(
  COMMAND tasklist /FI "IMAGENAME eq Win2Air.exe" /NH
  OUTPUT_VARIABLE running
  RESULT_VARIABLE tasklist_result)
if(NOT tasklist_result EQUAL 0)
  message(FATAL_ERROR "Could not check whether Win2Air is running.")
endif()
if(running MATCHES "Win2Air.exe")
  message(FATAL_ERROR "Exit Win2Air before capturing store screenshots.")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCONFIGURATION=${CONFIGURATION}
          -P "${CMAKE_CURRENT_LIST_DIR}/build-app.cmake"
  RESULT_VARIABLE app_result)
if(NOT app_result EQUAL 0)
  message(FATAL_ERROR "Store screenshot App build failed.")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -DCONFIGURATION=${CONFIGURATION} -DTESTS_ONLY=ON
          -P "${CMAKE_CURRENT_LIST_DIR}/build-app.cmake"
  RESULT_VARIABLE tests_result)
if(NOT tests_result EQUAL 0)
  message(FATAL_ERROR "Store screenshot test build failed.")
endif()
set(app "${APP_ROOT}/out/bin/${CONFIGURATION}/Win2Air.exe")
set(tests "${APP_ROOT}/out/tests/${CONFIGURATION}/Win2Air.Tests.exe")
set(output "${APP_ROOT}/out/store-screenshots")
if(EXISTS "${output}")
  file(REMOVE_RECURSE "${output}")
endif()
execute_process(
  COMMAND "${tests}" --store-screenshots "${app}" "${output}"
  RESULT_VARIABLE capture_result
  TIMEOUT 180)
if(NOT capture_result EQUAL 0)
  message(FATAL_ERROR "Store screenshot capture failed.")
endif()
foreach(name zh-CN-playback zh-CN-statistics zh-CN-settings en-US-playback
             en-US-statistics en-US-settings)
  message(STATUS "${output}/${name}.png")
endforeach()
