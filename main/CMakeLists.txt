set(srcs "main.c" "hid_host.c" "xinput.c" "input_bridge.c" "xinput_desc.c")

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ld_include_hid")