# Pocket Retro — the game's own build file.
# It says: "build main.c, and it needs the ESP-IDF GPIO, SPI and
# display parts." (In ESP-IDF v6 these live in esp_driver_gpio,
# esp_driver_spi and esp_lcd — the old single "driver" component
# no longer holds them.)
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_timer)
