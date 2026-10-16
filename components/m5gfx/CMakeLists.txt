set(M5GFX_ROOT "/home/kt/Paper_Mono/PaperMonoWakeDemo/lib/M5GFX/src")

file(GLOB_RECURSE M5GFX_SOURCES
    "${M5GFX_ROOT}/*.c"
    "${M5GFX_ROOT}/*.cpp"
)

idf_component_register(
    SRCS ${M5GFX_SOURCES}
    INCLUDE_DIRS "${M5GFX_ROOT}"
    REQUIRES driver esp_lcd efuse esp_timer esp_event esp_netif esp_partition freertos heap log nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    M5GFX_BOARD=34
)
