idf_component_register(SRCS "" INCLUDE_DIRS "")

# Ensure the spiffs directory exists with at least one file
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/spiffs")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/spiffs")
endif()

spiffs_create_partition_image(spiffs "${CMAKE_CURRENT_SOURCE_DIR}/spiffs" FLASH_IN_PROJECT)
