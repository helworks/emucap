// emucap producer hooks for Vita3K (see emucap_vita3k.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct EmuEnvState;
struct SDL_Window;

// Session thread, after the app session runs: connect to the Control MCP when managed.
void emucap_vita3k_start(EmuEnvState& emuenv);
void emucap_vita3k_stop();
// Display vblank thread, once per emulated vblank (outside the display locks).
void emucap_vita3k_vblank();
// Render thread, before each swap, with the OpenGL context current.
void emucap_vita3k_capture(SDL_Window* window);
