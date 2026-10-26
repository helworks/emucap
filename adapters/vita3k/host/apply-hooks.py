"""Apply the emucap hook edits to a Vita3K checkout (run from its root by build.sh)."""
from pathlib import Path

def edit(rel, old, new):
    p = Path(rel); t = p.read_text()
    assert t.count(old) == 1, (rel, old[:60])
    p.write_text(t.replace(old, new))

edit("vita3k/display/src/display.cpp",
     "static constexpr int TARGET_FPS = 60;\n",
     "// emucap: called once per emulated vblank, outside the display locks (null when unmanaged).\n"
     "void (*g_emucap_vblank_hook)() = nullptr;\n\n"
     "static constexpr int TARGET_FPS = 60;\n")
edit("vita3k/display/src/display.cpp",
     "        }\n        const auto time_ms = std::chrono::duration_cast",
     "        }\n        if (g_emucap_vblank_hook)\n            g_emucap_vblank_hook();\n"
     "        const auto time_ms = std::chrono::duration_cast")
edit("vita3k/ctrl/src/ctrl.cpp",
     "static void retrieve_ctrl_data(",
     "// emucap: buttons held by the Control MCP replace port 1 input while engaged.\n"
     "std::atomic<bool> g_emucap_ctrl_engaged{false};\n"
     "std::atomic<uint32_t> g_emucap_ctrl_buttons{0};\n\n"
     "static void retrieve_ctrl_data(")
edit("vita3k/ctrl/src/ctrl.cpp",
     "            apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);\n        }\n    }\n\n    reset_axes();\n}",
     "            apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);\n        }\n    }\n\n"
     "    if (port == 1 && g_emucap_ctrl_engaged.load(std::memory_order_acquire))\n"
     "        buttons = g_emucap_ctrl_buttons.load(std::memory_order_acquire);\n\n"
     "    reset_axes();\n}")
edit("vita3k/sdl-frontend/src/frame_host.cpp",
     "void FrameHost::swap_buffers() {\n    SDL_GL_SwapWindow(window);",
     "void FrameHost::swap_buffers() {\n    emucap_vita3k_capture(window);\n    SDL_GL_SwapWindow(window);")
edit("vita3k/sdl-frontend/src/frame_host.cpp",
     '#include "frame_host.h"\n',
     '#include "frame_host.h"\n#include "emucap_vita3k.h"\n')
edit("vita3k/sdl-frontend/src/session.cpp",
     "    int exit_code = 0;\n    bool relaunch_requested = false;\n",
     "    int exit_code = 0;\n    bool relaunch_requested = false;\n"
     "    // emucap: connect before the app boots; requests wait for the first emulated vblank.\n"
     "    emucap_vita3k_start(emuenv);\n")
p = Path("vita3k/sdl-frontend/src/session.cpp"); t = p.read_text()
first_include = t.index("#include")
p.write_text(t[:first_include] + '#include "emucap_vita3k.h"\n' + t[first_include:])
p = Path("vita3k/ctrl/src/ctrl.cpp"); t = p.read_text()
if "#include <atomic>" not in t:
    first_include = t.index("#include")
    p.write_text(t[:first_include] + "#include <atomic>\n" + t[first_include:])
print("ok")

edit("vita3k/sdl-frontend/CMakeLists.txt",
     "\tsrc/session.cpp\n)\n",
     "\tsrc/session.cpp\n\tsrc/emucap_vita3k.cpp\n\tsrc/emucap_vita3k.h\n\tsrc/emucap_producer.cpp\n\tsrc/emucap_producer.h\n)\n\n"
     "find_package(ZLIB REQUIRED)\n")
edit("vita3k/sdl-frontend/CMakeLists.txt",
     "target_link_libraries(sdl-frontend PRIVATE app audio config ctrl dialog ime motion renderer touch util SDL3::SDL3)",
     "target_link_libraries(sdl-frontend PRIVATE app audio config cpu ctrl dialog display ime kernel mem motion renderer touch util SDL3::SDL3 ZLIB::ZLIB)")
print("cmake ok")

# Ubuntu 26.04 ships Qt 6.10; upstream raised the minimum to 6.11 in a CI overhaul (cf37c744)
# without using 6.11 APIs in the code paths built here.
edit("cmake/qt6.cmake", "set(VITA3K_QT_MIN_VER 6.11.0)", "set(VITA3K_QT_MIN_VER 6.10.0)")
print("qt ok")
