cmake_minimum_required(VERSION 3.16)

# Firmware version: taken from version.txt in this folder (ESP-IDF reads it
# automatically when PROJECT_VER is not set). Edit that file to bump it; it
# is shown in the web UI and embedded in the .bin used for OTA.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_plug_control)
