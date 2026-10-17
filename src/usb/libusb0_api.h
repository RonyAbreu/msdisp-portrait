// Minimal dynamic binding to libusb-win32 (libusb0.dll, API 0.1). Only what msdisp needs.
// Struct layouts mirror lusb0_usb.h; we only touch fields whose offsets do not depend on packing.
#pragma once
#include <cstdint>

namespace msdisp::usb0 {

constexpr int kPathMax = 512;

struct usb_device_descriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct usb_bus;
struct usb_device {
    usb_device *next, *prev;
    char filename[kPathMax];
    usb_bus* bus;
    usb_device_descriptor descriptor;
    // remaining fields unused
};

struct usb_bus {
    usb_bus *next, *prev;
    char dirname[kPathMax];
    usb_device* devices;
    // remaining fields unused
};

struct usb_dev_handle;

struct Api {
    void(__cdecl* init)();
    int(__cdecl* find_busses)();
    int(__cdecl* find_devices)();
    usb_bus*(__cdecl* get_busses)();
    usb_dev_handle*(__cdecl* open)(usb_device*);
    int(__cdecl* close)(usb_dev_handle*);
    int(__cdecl* set_configuration)(usb_dev_handle*, int);
    int(__cdecl* claim_interface)(usb_dev_handle*, int);
    int(__cdecl* release_interface)(usb_dev_handle*, int);
    int(__cdecl* bulk_write)(usb_dev_handle*, int ep, char* bytes, int size, int timeout);
    int(__cdecl* clear_halt)(usb_dev_handle*, unsigned int ep);
    int(__cdecl* bulk_setup_async)(usb_dev_handle*, void** context, unsigned char ep);
    int(__cdecl* submit_async)(void* context, char* bytes, int size);
    int(__cdecl* reap_async_nocancel)(void* context, int timeout);
    int(__cdecl* cancel_async)(void* context);
    int(__cdecl* free_async)(void** context);
    char*(__cdecl* strerror)();
};

// Loads libusb0.dll once (System32 first). Returns nullptr if missing.
const Api* api();

}  // namespace msdisp::usb0
