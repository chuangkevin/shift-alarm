idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES arduino-esp32 arduino_libraries alarm_tailnet alarm_ota alarm_proxy nvs_flash driver)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CUBE_TFT=1 ARDUINO_USB_CDC_ON_BOOT=1 ARDUINO_USB_MODE=1 BOARD_HAS_PSRAM=1)
