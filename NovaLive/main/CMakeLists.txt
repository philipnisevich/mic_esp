if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/config.h")
  message(FATAL_ERROR "NovaLive/main/config.h is missing. Run: cp NovaLive/main/config.example.h NovaLive/main/config.h")
endif()

idf_component_register(
  SRCS "main.c" "audio.c" "wake.c" "session.c" "signaling.c" "display.c" "net.c" "serial.c" "tasks.c" "led.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_lcd esp_wifi
           esp_netif nvs_flash esp_http_client esp_timer esp-tls mbedtls esp_driver_rmt
)
