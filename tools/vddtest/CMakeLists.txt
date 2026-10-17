add_executable(msdisp-vddtest main.cpp)
target_link_options(msdisp-vddtest PRIVATE -municode)
target_link_libraries(msdisp-vddtest PRIVATE msdisp_vdd msdisp_capture gdi32 dwmapi)
