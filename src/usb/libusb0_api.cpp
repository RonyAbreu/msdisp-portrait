#include "libusb0_api.h"

#include <windows.h>

#include "msdisp/log.h"

namespace msdisp::usb0 {

namespace {
template <typename T>
bool bind(HMODULE m, const char* name, T& fn) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!fn) MSD_ERROR("libusb0.dll: missing export %s", name);
    return fn != nullptr;
}

Api* load() {
    HMODULE m = LoadLibraryExW(L"libusb0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        MSD_ERROR("libusb0.dll not found in System32 (error %lu)", GetLastError());
        return nullptr;
    }
    static Api a{};
    bool ok = bind(m, "usb_init", a.init) && bind(m, "usb_find_busses", a.find_busses) &&
              bind(m, "usb_find_devices", a.find_devices) && bind(m, "usb_get_busses", a.get_busses) &&
              bind(m, "usb_open", a.open) && bind(m, "usb_close", a.close) &&
              bind(m, "usb_set_configuration", a.set_configuration) &&
              bind(m, "usb_claim_interface", a.claim_interface) &&
              bind(m, "usb_release_interface", a.release_interface) && bind(m, "usb_bulk_write", a.bulk_write) &&
              bind(m, "usb_clear_halt", a.clear_halt) && bind(m, "usb_bulk_setup_async", a.bulk_setup_async) &&
              bind(m, "usb_submit_async", a.submit_async) &&
              bind(m, "usb_reap_async_nocancel", a.reap_async_nocancel) &&
              bind(m, "usb_cancel_async", a.cancel_async) && bind(m, "usb_free_async", a.free_async) &&
              bind(m, "usb_strerror", a.strerror);
    if (!ok) return nullptr;
    a.init();
    return &a;
}
}  // namespace

const Api* api() {
    static Api* a = load();
    return a;
}

}  // namespace msdisp::usb0
