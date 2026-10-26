"""Apply the emucap hook edits to a Xenia Canary checkout (run from its root)."""
from pathlib import Path


def edit(rel, old, new, count=1):
    p = Path(rel)
    t = p.read_text()
    assert t.count(old) == count, (rel, old[:70], t.count(old))
    p.write_text(t.replace(old, new))


# Frame boundary: after every guest swap packet, once IssueSwap refreshed the guest output.
edit("src/xenia/gpu/command_processor.h",
     "class CommandProcessor {\n",
     "// emucap: called on the command processor thread after every guest swap (null unless a\n"
     "// managed launch installed the producer).\n"
     "extern void (*g_emucap_swap_hook)();\n\n"
     "class CommandProcessor {\n")
edit("src/xenia/gpu/command_processor.cc",
     "namespace xe {\nnamespace gpu {\n",
     "namespace xe {\nnamespace gpu {\n\nvoid (*g_emucap_swap_hook)() = nullptr;\n")
edit("src/xenia/gpu/pm4_command_processor_implement.h",
     "  COMMAND_PROCESSOR::IssueSwap(frontbuffer_ptr, frontbuffer_width,\n"
     "                               frontbuffer_height);\n\n"
     "  ++counter_;\n",
     "  COMMAND_PROCESSOR::IssueSwap(frontbuffer_ptr, frontbuffer_width,\n"
     "                               frontbuffer_height);\n\n"
     "  ++counter_;\n"
     "  if (g_emucap_swap_hook) {\n"
     "    g_emucap_swap_hook();\n"
     "  }\n")

# Input: the producer owns user 0 while it is connected.
edit("src/xenia/hid/input_system.cc",
     "namespace xe {\nnamespace hid {\n",
     "namespace xe {\nnamespace hid {\n\n"
     "bool (*g_emucap_pad_hook)(uint32_t user_index, uint16_t* buttons) = nullptr;\n")
edit("src/xenia/hid/input_system.cc",
     "X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,\n"
     "                                      X_INPUT_CAPABILITIES* out_caps) {\n"
     "  SCOPE_profile_cpu_f(\"hid\");\n",
     "X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,\n"
     "                                      X_INPUT_CAPABILITIES* out_caps) {\n"
     "  SCOPE_profile_cpu_f(\"hid\");\n"
     "  uint16_t emucap_buttons = 0;\n"
     "  if (g_emucap_pad_hook && g_emucap_pad_hook(user_index, &emucap_buttons)) {\n"
     "    std::memset(out_caps, 0, sizeof(*out_caps));\n"
     "    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD\n"
     "    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD\n"
     "    out_caps->gamepad.buttons = 0xF7FF;\n"
     "    out_caps->gamepad.left_trigger = 0xFF;\n"
     "    out_caps->gamepad.right_trigger = 0xFF;\n"
     "    return X_ERROR_SUCCESS;\n"
     "  }\n")
edit("src/xenia/hid/input_system.cc",
     "X_RESULT InputSystem::GetState(uint32_t user_index, uint32_t flags,\n"
     "                               X_INPUT_STATE* out_state) {\n"
     "  SCOPE_profile_cpu_f(\"hid\");\n",
     "X_RESULT InputSystem::GetState(uint32_t user_index, uint32_t flags,\n"
     "                               X_INPUT_STATE* out_state) {\n"
     "  SCOPE_profile_cpu_f(\"hid\");\n"
     "  uint16_t emucap_buttons = 0;\n"
     "  if (g_emucap_pad_hook && g_emucap_pad_hook(user_index, &emucap_buttons)) {\n"
     "    static uint16_t emucap_previous = 0;\n"
     "    static uint32_t emucap_packet = 1;\n"
     "    if (emucap_buttons != emucap_previous) {\n"
     "      emucap_previous = emucap_buttons;\n"
     "      emucap_packet++;\n"
     "    }\n"
     "    std::memset(out_state, 0, sizeof(*out_state));\n"
     "    out_state->packet_number = emucap_packet;\n"
     "    out_state->gamepad.buttons = emucap_buttons;\n"
     "    return X_ERROR_SUCCESS;\n"
     "  }\n")
t = Path("src/xenia/hid/input_system.cc").read_text()
if "#include <cstring>" not in t:
    edit("src/xenia/hid/input_system.cc",
         '#include "xenia/hid/input_system.h"\n',
         '#include "xenia/hid/input_system.h"\n\n#include <cstring>\n')

# Producer sources and zlib (PNG encoding) in the xenia_canary executable.
edit("src/xenia/app/CMakeLists.txt",
     "target_sources(xenia-app PRIVATE\n  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc\n)\n",
     "target_sources(xenia-app PRIVATE\n  ${CMAKE_CURRENT_SOURCE_DIR}/xenia_main.cc\n"
     "  ${CMAKE_CURRENT_SOURCE_DIR}/emucap/emucap_xenia.cpp\n"
     "  ${CMAKE_CURRENT_SOURCE_DIR}/emucap/emucap_producer.cpp\n)\n"
     "find_package(ZLIB REQUIRED)\n"
     "target_link_libraries(xenia-app PRIVATE ZLIB::ZLIB)\n")
print("ok")
