"""Apply the emucap hook edits to an RPCS3 checkout (run from its root)."""
from pathlib import Path


def edit(rel, old, new, count=1):
    p = Path(rel)
    t = p.read_text()
    assert t.count(old) == count, (rel, old[:70], t.count(old))
    p.write_text(t.replace(old, new))


edit("rpcs3/Emu/RSX/RSXThread.h",
     "\tpublic:\n\t\tatomic_t<u64> new_get_put = u64{umax};\n",
     "\tpublic:\n\t\t// emucap: arm RPCS3's pause-after-N-flips frame advance.\n"
     "\t\tvoid emucap_pause_after_flips(u32 count) { m_pause_after_x_flips = count; }\n\n"
     "\t\tatomic_t<u64> new_get_put = u64{umax};\n")
edit("rpcs3/Emu/RSX/RSXThread.cpp",
     "\t\tif (info.emu_flip)\n\t\t{\n\t\t\tperformance_counters.sampled_frames++;\n",
     "\t\tif (info.emu_flip)\n\t\t{\n\t\t\tperformance_counters.sampled_frames++;\n"
     "\t\t\temucap_rpcs3_on_flip(this, info.buffer);\n")
edit("rpcs3/Emu/RSX/RSXThread.cpp",
     "#include <unordered_set>\n",
     "#include <unordered_set>\n\n#include \"Emu/emucap/emucap_rpcs3.h\"\n")
edit("rpcs3/Emu/Cell/Modules/cellPad.cpp",
     "\t\tif (d1_initial != output.button[CELL_PAD_BTN_OFFSET_DIGITAL1] || d2_initial != output.button[CELL_PAD_BTN_OFFSET_DIGITAL2])\n",
     "\t\t// emucap: the Control MCP owns port 0 digital buttons while its producer is connected.\n"
     "\t\tif (port_no == 0 && g_emucap_pad_active.load())\n\t\t{\n"
     "\t\t\toutput.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = static_cast<u16>(g_emucap_pad_digital1.load());\n"
     "\t\t\toutput.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = static_cast<u16>(g_emucap_pad_digital2.load());\n"
     "\t\t}\n\n"
     "\t\tif (d1_initial != output.button[CELL_PAD_BTN_OFFSET_DIGITAL1] || d2_initial != output.button[CELL_PAD_BTN_OFFSET_DIGITAL2])\n")
edit("rpcs3/Emu/Cell/Modules/cellPad.cpp",
     '#include "cellPad.h"\n',
     '#include "cellPad.h"\n#include "Emu/emucap/emucap_rpcs3.h"\n')
edit("rpcs3/rpcs3.cpp",
     "#include \"util/sysinfo.hpp\"\n",
     "#include \"util/sysinfo.hpp\"\n#include \"Emu/emucap/emucap_rpcs3.h\"\n")
edit("rpcs3/rpcs3.cpp",
     "\t\treturn gui_app->exec();\n",
     "\t\temucap_rpcs3_start(\"\");\n\t\treturn gui_app->exec();\n")
edit("rpcs3/rpcs3.cpp",
     "\treturn app->exec();\n",
     "\temucap_rpcs3_start(\"\");\n\treturn app->exec();\n")
edit("rpcs3/Emu/CMakeLists.txt",
     "add_library(rpcs3_emu STATIC\n",
     "add_library(rpcs3_emu STATIC\n    emucap/emucap_rpcs3.cpp\n    emucap/emucap_producer.cpp\n")
print("ok")

edit("rpcs3/Emu/CMakeLists.txt",
     "add_library(rpcs3_emu STATIC\n",
     "set_source_files_properties(emucap/emucap_rpcs3.cpp emucap/emucap_producer.cpp PROPERTIES COMPILE_OPTIONS \"-w;-Wno-error;-fexceptions\")\n\nadd_library(rpcs3_emu STATIC\n")
print("cmake ok")

edit("rpcs3/Emu/RSX/RSXThread.cpp",
     "\t\t\tif (m_pause_after_x_flips && m_pause_after_x_flips-- == 1)\n\t\t\t{\n\t\t\t\tEmu.Pause();\n",
     "\t\t\tif (m_pause_after_x_flips && m_pause_after_x_flips-- == 1)\n\t\t\t{\n"
     "\t\t\t\t// emucap: a frame-advance stop, without the resume message overlay.\n"
     "\t\t\t\tEmu.Pause(false, false);\n")
print("pause ok")
