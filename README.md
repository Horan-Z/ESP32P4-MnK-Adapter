## ESP32P4-MnK-Adapter

Based on CherryUSB.

Tested on Waveshare ESP32-P4-WIFI6-DEV-KIT.

Manual configuration in ```xinput.c``` is required to:
- Define your own mapping (currently preset for Apex with my own preferences).
- Adjust the parsing logic to match your mouse's specific HID report format.

You might also need to increase the ```CONFIG_USBHOST_MAX_HID_CLASS``` located in ```managed_components/cherry-embedded__cherryusb/osal/idf/usb_config.h``` because modern MnK devices usually have multiple HID interfaces.
