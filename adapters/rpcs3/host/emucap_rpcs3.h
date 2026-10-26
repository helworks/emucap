// emucap producer hooks for RPCS3 (see emucap_rpcs3.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rsx { class thread; }

// Held buttons for cellPad port 0 (active once the producer is connected).
extern std::atomic<bool> g_emucap_pad_active;
extern std::atomic<uint32_t> g_emucap_pad_digital1;
extern std::atomic<uint32_t> g_emucap_pad_digital2;

// Main thread, before the game boots: connect to the Control MCP when managed.
void emucap_rpcs3_start(const std::string& content);
// RSX thread, once per emulated flip.
void emucap_rpcs3_on_flip(rsx::thread* rsx, unsigned buffer);
