add_executable(verify main.cpp)
target_link_libraries(verify PRIVATE msdisp_vdd msdisp_capture msdisp_gpu)
