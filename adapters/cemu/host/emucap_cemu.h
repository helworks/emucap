// emucap producer hooks for Cemu (see emucap_cemu.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

// Held Wii U GamePad buttons (VPAD hold bits) while the producer is connected.
extern std::atomic<bool> g_emucap_vpad_active;
extern std::atomic<uint32_t> g_emucap_vpad_hold;

// GPU thread, at each TV buffer swap, with the TV frame read back (RGB8, bottom-up rows).
// Starts the producer on first use and parks here while the guest is frozen.
void emucap_cemu_tv_swap(int width, int height, const uint8_t* rgb_bottom_up);
bool emucap_cemu_active();
