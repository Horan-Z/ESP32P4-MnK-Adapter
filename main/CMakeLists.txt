set(srcs "main.c")

if(CONFIG_CHERRYUSB_HOST_HID)
    list(APPEND srcs "hid_host.c" "xinput.c" "input_bridge.c" "xinput_desc.c")
endif()

idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer fatfs esp_ringbuf
)

if(CONFIG_CHERRYUSB_HOST_HID)
    # Make sure the definitions in hid.c are linked correctly
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ld_include_hid")
endif()