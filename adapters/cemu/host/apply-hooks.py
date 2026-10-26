"""Apply the emucap hook edits to a Cemu checkout (run from its root)."""
from pathlib import Path


def edit(rel, old, new, count=1):
    p = Path(rel)
    t = p.read_text()
    assert t.count(old) == count, (rel, old[:70], t.count(old))
    p.write_text(t.replace(old, new))


edit("src/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.cpp",
     '#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"\n',
     '#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"\n#include "Cafe/emucap/emucap_cemu.h"\n')
edit("src/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.cpp",
     "void OpenGLRenderer::SwapBuffers(bool swapTV, bool swapDRC)\n{\n\tGLCanvas_SwapBuffers(swapTV, swapDRC);\n",
     "void OpenGLRenderer::SwapBuffers(bool swapTV, bool swapDRC)\n{\n"
     "\t// emucap: in a managed launch, read the TV frame back and offer a frame boundary.\n"
     "\tstatic const bool emucap_managed = std::getenv(\"EMUCAP_PORT\") != nullptr;\n"
     "\tif (swapTV && emucap_managed)\n\t{\n"
     "\t\tstatic std::vector<uint8> emucap_rgb;\n"
     "\t\t// The whole TV window back buffer, not the last viewport.\n"
     "\t\tint width = 0, height = 0;\n"
     "\t\tgui_getWindowPhysSize(width, height);\n"
     "\t\tif (width > 0 && height > 0 && width <= 8192 && height <= 8192)\n\t\t{\n"
     "\t\t\temucap_rgb.resize((size_t)width * height * 3);\n"
     "\t\t\tglReadBuffer(GL_BACK);\n"
     "\t\t\tglPixelStorei(GL_PACK_ALIGNMENT, 1);\n"
     "\t\t\tglReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, emucap_rgb.data());\n"
     "\t\t\temucap_cemu_tv_swap(width, height, emucap_rgb.data());\n"
     "\t\t}\n\t\telse\n\t\t\temucap_cemu_tv_swap(0, 0, nullptr);\n"
     "\t}\n"
     "\tGLCanvas_SwapBuffers(swapTV, swapDRC);\n")
edit("src/Cafe/OS/libs/vpad/vpad.cpp",
     '#include "input/InputManager.h"\n',
     '#include "input/InputManager.h"\n#include "Cafe/emucap/emucap_cemu.h"\n')
edit("src/Cafe/OS/libs/vpad/vpad.cpp",
     "\t\t\tcontroller->VPADRead(*status, vpad::g_vpad.controller_data[channel].btn_repeat);\n",
     "\t\t\tcontroller->VPADRead(*status, vpad::g_vpad.controller_data[channel].btn_repeat);\n"
     "\t\t\t// emucap: the Control MCP owns GamePad buttons while its producer is connected.\n"
     "\t\t\tif (channel == 0 && g_emucap_vpad_active.load())\n\t\t\t{\n"
     "\t\t\t\tstatic uint32 emucap_previous = 0;\n"
     "\t\t\t\tconst uint32 hold = g_emucap_vpad_hold.load();\n"
     "\t\t\t\tstatus->hold = hold;\n"
     "\t\t\t\tstatus->trig = hold & ~emucap_previous;\n"
     "\t\t\t\tstatus->release = emucap_previous & ~hold;\n"
     "\t\t\t\temucap_previous = hold;\n"
     "\t\t\t}\n")
edit("src/Cafe/CMakeLists.txt",
     "add_library(CemuCafe\n",
     "add_library(CemuCafe\n  emucap/emucap_cemu.cpp\n  emucap/emucap_producer.cpp\n")
print("ok")

p = Path("src/Common/GLInclude/glFunctions.h"); t = p.read_text()
add = ""
for proc, name in (("PFNGLREADBUFFERPROC", "glReadBuffer"), ("PFNGLREADPIXELSPROC", "glReadPixels"), ("PFNGLPIXELSTOREIPROC", "glPixelStorei")):
    if f", {name})" not in t:
        add += f"GLFUNC({proc}, {name})\n"
anchor = "GLFUNC(PFNGLGETINTEGERVPROC, glGetIntegerv)\n"
assert t.count(anchor) == 1
p.write_text(t.replace(anchor, anchor + add))
print("gl ok")
