set(M5UNIFIED_ROOT "/home/kt/Paper_Mono/PaperMonoWakeDemo/lib/M5Unified/src")

file(GLOB_RECURSE M5UNIFIED_SOURCES
    "${M5UNIFIED_ROOT}/*.c"
    "${M5UNIFIED_ROOT}/*.cpp"
)

idf_component_register(
    SRCS ${M5UNIFIED_SOURCES}
    INCLUDE_DIRS "${M5UNIFIED_ROOT}"
    REQUIRES m5gfx driver esp_adc esp_timer esp_event esp_netif nvs_flash
)
