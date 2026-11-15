# Reproducible standalone desktop library: embed the VNC core and use only OS
# desktop libraries at runtime. Pass with cmake -C cmake/DesktopBuild.cmake.
set(WITH_DESKTOP_SERVER ON CACHE BOOL "")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(WITH_LIBVNCSERVER ON CACHE BOOL "")
set(WITH_LIBVNCCLIENT OFF CACHE BOOL "")
set(WITH_THREADS ON CACHE BOOL "")
set(PREFER_WIN32THREADS ON CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
foreach(feature ZLIB LZO JPEG PNG SDL GTK LIBSSHTUNNEL GNUTLS OPENSSL SYSTEMD
                GCRYPT FFMPEG TIGHTVNC_FILETRANSFER WEBSOCKETS SASL XCB EXAMPLES QT)
  set(WITH_${feature} OFF CACHE BOOL "")
endforeach()
