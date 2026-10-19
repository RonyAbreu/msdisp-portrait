add_executable(desktopicons main.cpp)
target_link_options(desktopicons PRIVATE -municode)
target_link_libraries(desktopicons PRIVATE msdisp_appcore)
