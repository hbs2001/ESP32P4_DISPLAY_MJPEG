set(srcs "ppa_dsi_main.c")

if(CONFIG_EXAMPLE_SOURCE_IMAGE_FORMAT_RGB565)
    list(APPEND srcs "image.c")
endif()

set(embed_files)

# if(CONFIG_EXAMPLE_SOURCE_IMAGE_FORMAT_JPEG)
#     list(APPEND embed_files "test.jpg")
# endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES esp_mm esp_driver_ppa dsi_init spiffs esp_timer
                       EMBED_FILES ${embed_files}
                       )
spiffs_create_partition_image(
                        storage
                        ../spiffs
                        FLASH_IN_PROJECT
                        USE_SPIFFS
                        VERBOSE
                    )