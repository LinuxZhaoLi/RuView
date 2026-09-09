# ESP32 Hello World — Capability Discovery (S3 / C6 targets)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-hello-world)
# 规避 GCC14.2 IRA pass segfault esp_lcd_panel_rgb.c
idf_component_get_property(lcd_lib esp_lcd COMPONENT_LIB)
target_compile_options(${lcd_lib} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O1>
)
