# emucap headless producer for the melonDS core. Added to the melonDS work tree by build.sh.
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
add_executable(emucap-melonds-host emucap_melonds.cpp emucap_producer.cpp)
target_link_libraries(emucap-melonds-host PRIVATE core ZLIB::ZLIB Threads::Threads)
target_include_directories(emucap-melonds-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
if (EMUCAP_BUILD_ID)
    target_compile_definitions(emucap-melonds-host PRIVATE "EMUCAP_BUILD_ID=\"${EMUCAP_BUILD_ID}\"")
endif()
