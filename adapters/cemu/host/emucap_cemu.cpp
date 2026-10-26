// emucap producer integration for Cemu (Wii U).
//
// build.sh copies this file into the Cemu work tree (src/Cafe) and applies the hook patch. Cemu has
// no emulator-wide pause, so the frame boundary is the TV buffer swap on the GPU thread: a frozen
// stop parks the GPU thread there, and the PPC threads block on GX2 shortly after. The OpenGL
// renderer reads the TV frame back before each swap, and VPADRead reports the held buttons.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/precompiled.h"
#include "emucap_cemu.h"

#include "emucap_producer.h"

#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/PPCState.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#if __has_include("emucap_build_id.h")
#include "emucap_build_id.h"
#endif
#ifndef EMUCAP_BUILD_ID
#define EMUCAP_BUILD_ID "unknown"
#endif

std::atomic<bool> g_emucap_vpad_active{false};
std::atomic<uint32_t> g_emucap_vpad_hold{0};

namespace {

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Json;
using emucap_producer::Region;

struct Button { const char* name; uint32_t bit; };
// VPAD hold bits (Wii U GamePad).
const Button kButtons[] = {
	{"a", 0x8000}, {"b", 0x4000}, {"x", 0x2000}, {"y", 0x1000},
	{"left", 0x0800}, {"right", 0x0400}, {"up", 0x0200}, {"down", 0x0100},
	{"zl", 0x0080}, {"zr", 0x0040}, {"l", 0x0020}, {"r", 0x0010},
	{"plus", 0x0008}, {"minus", 0x0004},
};

std::mutex g_frame_mutex;
FrameImage g_frame;

class CemuBackend : public emucap_producer::Backend {
public:
	explicit CemuBackend(std::string content) : content_(std::move(content)) {
		auto read = [](uint64_t a, uint8_t* out, size_t n) {
			if (n == 0 || a + n > (1ull << 32)) return false;
			if (!memory_isAddressRangeAccessible((MPTR)a, (uint32)n)) return false;
			std::memcpy(out, memory_getPointerFromVirtualOffset((uint32)a), n);
			return true;
		};
		auto write = [](uint64_t a, const uint8_t* in, size_t n) {
			if (n == 0 || a + n > (1ull << 32)) return false;
			if (!memory_isAddressRangeAccessible((MPTR)a, (uint32)n)) return false;
			std::memcpy(memory_getPointerFromVirtualOffset((uint32)a), in, n);
			return true;
		};
		// The Wii U guest effective address space (MEM2 at 0x10000000.., MEM1 at 0xF4000000..).
		regions_.push_back({"vm", 1ull << 32, true, false, read, write});
	}

	std::string system() const override { return "wiiu"; }
	std::string adapter() const override { return "cemu"; }
	std::string build() const override { return EMUCAP_BUILD_ID; }
	std::string content_path() const override { return content_; }
	std::vector<Region>& regions() override { return regions_; }
	std::vector<std::string> button_names() const override {
		std::vector<std::string> names;
		for (const auto& b : kButtons) names.push_back(b.name);
		return names;
	}
	void set_buttons(uint64_t mask) override {
		uint32_t bits = 0;
		for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
			if (mask & (1ull << i)) bits |= kButtons[i].bit;
		g_emucap_vpad_hold = bits;
	}
	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		CpuState s;
		s.cpu_id = "ppc";
		s.aliases = {"main", "cpu"};
		out.push_back(std::move(s));
	}
	bool supports_state() const override { return false; }
	bool save_state(std::vector<uint8_t>&, std::string& error) override { error = "Cemu has no savestates"; return false; }
	bool load_state(const std::vector<uint8_t>&, std::string& error) override { error = "Cemu has no savestates"; return false; }
	bool capture(FrameImage& image, std::string& error) override {
		const std::lock_guard<std::mutex> lock(g_frame_mutex);
		if (g_frame.rgb.empty()) { error = "no TV frame has been presented yet"; return false; }
		image = g_frame;
		return true;
	}
	bool controlled_start() const override { return false; }
	std::vector<std::string> active_exceptions() const override {
		return {"cemu.execution.instruction-step-absent", "cemu.input-hold.port-zero-only"};
	}
	void extend_hello(Json& hello) const override {
		Json notes = Json::object();
		notes.set("frames", Json("step counts TV buffer swaps on the GPU thread"));
		notes.set("frozen", Json("best effort: the GPU thread parks at the swap and PPC threads block on GX2 shortly after; Cemu has no emulator-wide pause"));
		notes.set("savestates", Json("Cemu has no savestates"));
		notes.set("screenshot", Json("TV output read back from the OpenGL renderer before the swap"));
		notes.set("input", Json("Wii U GamePad (VPAD channel 0)"));
		hello.set("capability_notes", notes);
	}

private:
	std::string content_;
	std::vector<Region> regions_;
};

std::unique_ptr<CemuBackend> g_backend;
std::unique_ptr<emucap_producer::Producer> g_producer;
// Connect at process start so launch readiness does not wait for the first TV frame; requests
// other than hello/status/get_rom_info wait for the first frame boundary.
struct AutoStart {
	AutoStart() {
		if (!std::getenv("EMUCAP_PORT")) return;
		const char* content = std::getenv("EMUCAP_CONTENT");
		g_backend = std::make_unique<CemuBackend>(content ? content : "");
		g_producer = std::make_unique<emucap_producer::Producer>(*g_backend);
		if (!g_producer->start()) {
			g_producer.reset();
			g_backend.reset();
			return;
		}
		g_emucap_vpad_active = true;
	}
} g_autostart;

}  // namespace

void emucap_cemu_tv_swap(int width, int height, const uint8_t* rgb_bottom_up) {
	if (!g_producer) return;
	if (rgb_bottom_up && width > 0 && height > 0) {
		const std::lock_guard<std::mutex> lock(g_frame_mutex);
		g_frame.width = width;
		g_frame.height = height;
		g_frame.rgb.resize((size_t)width * height * 3);
		for (int y = 0; y < height; y++)
			std::memcpy(&g_frame.rgb[(size_t)y * width * 3], rgb_bottom_up + (size_t)(height - 1 - y) * width * 3, (size_t)width * 3);
	}
	g_producer->boundary();
}

bool emucap_cemu_active() { return g_producer != nullptr; }
