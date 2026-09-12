cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.2.5")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xingzhi-cube-1.54tft-wifi)
idf_component_get_property(arduino_core espressif__arduino-esp32 COMPONENT_LIB)
target_compile_definitions(${arduino_core} PUBLIC ARDUINO_USB_CDC_ON_BOOT=1 ARDUINO_USB_MODE=1 BOARD_HAS_PSRAM=1)

# Applied after IDF resolves managed components, before compiler reads sources.
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/patches/apply_arduino_http.py"
  "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__arduino-esp32"
  RESULT_VARIABLE alarm_http_patch_result)
if(NOT alarm_http_patch_result EQUAL 0)
  message(FATAL_ERROR "Reviewed Arduino HTTP parser patch could not be applied")
endif()

execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/patches/apply_network_bind.py"
  "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__arduino-esp32"
  RESULT_VARIABLE alarm_bind_patch_result)
if(NOT alarm_bind_patch_result EQUAL 0)
  message(FATAL_ERROR "Reviewed loopback bind patch could not be applied")
endif()
