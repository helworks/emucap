# Shim for the distro hidapi package, which ships no CMake config.
if (NOT TARGET hidapi::hidapi)
  add_library(hidapi::hidapi UNKNOWN IMPORTED)
  set_target_properties(hidapi::hidapi PROPERTIES
    IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libhidapi-hidraw.so"
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include/hidapi")
endif()
set(hidapi_FOUND TRUE)
