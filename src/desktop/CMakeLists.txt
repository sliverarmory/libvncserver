if(CMAKE_VERSION VERSION_LESS 3.16)
  message(FATAL_ERROR "WITH_DESKTOP_SERVER requires CMake 3.16 or newer")
endif()
if(NOT WITH_LIBVNCSERVER OR NOT WITH_THREADS OR NOT Threads_FOUND)
  message(FATAL_ERROR "WITH_DESKTOP_SERVER requires WITH_LIBVNCSERVER and WITH_THREADS")
endif()
if(WITH_SYSTEMD OR SYSTEMD_FOUND)
  message(FATAL_ERROR "Use WITH_SYSTEMD=OFF for the loopback-only desktop server")
endif()

set(desktop_libraries vncserver Threads::Threads)
if(WIN32)
  set(desktop_source windows.c)
  list(APPEND desktop_libraries user32 gdi32)
elseif(APPLE)
  if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.0)
    message(FATAL_ERROR "The desktop backend requires CMAKE_OSX_DEPLOYMENT_TARGET >= 14.0")
  endif()
  enable_language(OBJC)
  set(desktop_source macos.m)
  set_source_files_properties(macos.m PROPERTIES COMPILE_FLAGS "-fobjc-arc -fblocks")
  list(APPEND desktop_libraries "-framework Foundation" "-framework ScreenCaptureKit"
       "-framework CoreGraphics" "-framework ApplicationServices")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DESKTOP_XCB REQUIRED IMPORTED_TARGET xcb xcb-xtest)
  set(desktop_source x11.c)
  list(APPEND desktop_libraries PkgConfig::DESKTOP_XCB)
endif()

# Embed the static core into a single loadable library in the DesktopBuild profile.
set_target_properties(vncserver PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT BUILD_SHARED_LIBS)
  set_target_properties(vncserver PROPERTIES C_VISIBILITY_PRESET hidden)
endif()

function(configure_desktop_target target)
  set_target_properties(${target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
      C_VISIBILITY_PRESET hidden OBJC_VISIBILITY_PRESET hidden
      WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  target_compile_definitions(${target} PRIVATE VNCDESKTOP_BUILD)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0601)
  endif()
  target_link_libraries(${target} PRIVATE ${desktop_libraries})
endfunction()

add_library(vncdesktop SHARED server.c sliver.c ${desktop_source})
configure_desktop_target(vncdesktop)
if(LIBVNCSERVER_INSTALL)
  install(TARGETS vncdesktop RUNTIME DESTINATION bin COMPONENT desktop
          LIBRARY DESTINATION lib COMPONENT desktop
          ARCHIVE DESTINATION lib COMPONENT desktop)
  install(FILES ${PROJECT_SOURCE_DIR}/include/rfb/desktop.h
          DESTINATION include/rfb COMPONENT desktop)
  install(FILES ${PROJECT_SOURCE_DIR}/doc/desktop-library.md ${PROJECT_SOURCE_DIR}/COPYING
          DESTINATION share/vncdesktop COMPONENT desktop)
endif()

if(WITH_TESTS)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  # A separate, non-installed fixture exercises the identical server/ABI without
  # needing desktop permissions. Production vncdesktop always uses a real backend.
  add_library(vncdesktop_fixture SHARED server.c sliver.c ${PROJECT_SOURCE_DIR}/test/desktop_fixture.c)
  configure_desktop_target(vncdesktop_fixture)
  target_include_directories(vncdesktop_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME desktop_protocol COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/test/desktop_smoke.py $<TARGET_FILE:vncdesktop_fixture> --fixture)
  add_test(NAME desktop_native COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/test/desktop_smoke.py $<TARGET_FILE:vncdesktop> --allow-unavailable)
  add_test(NAME desktop_sliver_abi COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/test/desktop_sliver.py $<TARGET_FILE:vncdesktop_fixture>)
  set_tests_properties(desktop_protocol desktop_native PROPERTIES TIMEOUT 90)
  set_tests_properties(desktop_sliver_abi PROPERTIES TIMEOUT 90)
  set_tests_properties(desktop_native PROPERTIES SKIP_RETURN_CODE 77)
  if(UNIX AND NOT APPLE)
    add_executable(desktop_x11_test ${PROJECT_SOURCE_DIR}/test/desktop_x11.c x11.c)
    target_include_directories(desktop_x11_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(desktop_x11_test PRIVATE PkgConfig::DESKTOP_XCB)
    set_target_properties(desktop_x11_test PROPERTIES C_STANDARD 99)
    add_test(NAME desktop_x11_input COMMAND desktop_x11_test)
    set_tests_properties(desktop_x11_input PROPERTIES TIMEOUT 30)
  elseif(WIN32)
    add_executable(desktop_windows_test ${PROJECT_SOURCE_DIR}/test/desktop_windows.c windows.c)
    target_include_directories(desktop_windows_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(desktop_windows_test PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(desktop_windows_test PRIVATE user32 gdi32)
    set_target_properties(desktop_windows_test PROPERTIES C_STANDARD 99)
    add_test(NAME desktop_windows_input COMMAND desktop_windows_test)
    set_tests_properties(desktop_windows_input PROPERTIES TIMEOUT 30)
  endif()
endif()
