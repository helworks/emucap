// emucap producer integration for Vita3K's SDL frontend.
//
// build.sh copies this file into the Vita3K work tree (sdl-frontend) and applies the hook patches:
// the display vblank thread calls emucap_vita3k_vblank() once per emulated vblank, FrameHost reads
// back the presented OpenGL frame before each swap, and the controller module honours the held
// emucap buttons. A frozen stop parks the vblank thread and pauses every guest kernel thread.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "emucap_vita3k.h"

#include "emucap_producer.h"

#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <cpu/functions.h>
#include <display/state.h>

#include <SDL3/SDL_video.h>

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

// Defined by the hook patches in the controller and display modules.
extern std::atomic<bool> g_emucap_ctrl_engaged;
extern std::atomic<uint32_t> g_emucap_ctrl_buttons;
extern void (*g_emucap_vblank_hook)();

namespace {

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Json;
using emucap_producer::Region;

struct Button { const char* name; uint32_t bit; };
// SceCtrl button bits (ctrl/ctrl.h).
const Button kButtons[] = {
	{"cross", 0x4000}, {"circle", 0x2000}, {"square", 0x8000}, {"triangle", 0x1000},
	{"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020},
	{"down", 0x0040}, {"left", 0x0080}, {"l", 0x0400}, {"r", 0x0800},
};

std::mutex g_frame_mutex;
FrameImage g_frame;
std::atomic<uint64_t> g_presented{0};

using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
using PixelStore = void (*)(unsigned, int);
using ReadBuffer = void (*)(unsigned);

class VitaBackend : public emucap_producer::Backend {
public:
	VitaBackend(EmuEnvState& emuenv, std::string content) : emuenv_(emuenv), content_(std::move(content)) {
		// The PS Vita user address space. Reads and writes are limited to allocated pages.
		regions_.push_back({"user", 1ull << 32, true, false,
			[this](uint64_t a, uint8_t* out, size_t n) {
				if (n == 0 || a + n > (1ull << 32)) return false;
				if (!is_valid_addr_range(emuenv_.mem, (Address)a, (Address)(a + n))) return false;
				std::memcpy(out, Ptr<uint8_t>((Address)a).get(emuenv_.mem), n);
				return true;
			},
			[this](uint64_t a, const uint8_t* in, size_t n) {
				if (n == 0 || a + n > (1ull << 32)) return false;
				if (!is_valid_addr_range(emuenv_.mem, (Address)a, (Address)(a + n))) return false;
				std::memcpy(Ptr<uint8_t>((Address)a).get(emuenv_.mem), in, n);
				return true;
			}});
	}

	std::string system() const override { return "psvita"; }
	std::string adapter() const override { return "vita3k"; }
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
		g_emucap_ctrl_buttons.store(bits);
		g_emucap_ctrl_engaged.store(mask != 0);
	}
	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		CpuState s;
		s.cpu_id = "main_thread";
		s.aliases = {"main", "cpu"};
		const std::lock_guard<std::mutex> lock(emuenv_.kernel.mutex);
		if (!emuenv_.kernel.threads.empty()) {
			auto& thread = emuenv_.kernel.threads.begin()->second;
			if (thread && thread->cpu) {
				for (int i = 0; i < 13; i++) s.registers.emplace_back("cpu.r" + std::to_string(i), read_reg(*thread->cpu, i));
				s.registers.emplace_back("cpu.sp", read_sp(*thread->cpu));
				s.registers.emplace_back("cpu.pc", read_pc(*thread->cpu));
				s.registers.emplace_back("thread.id", (uint64_t)emuenv_.kernel.threads.begin()->first);
			}
		}
		s.registers.emplace_back("kernel.threads", (uint64_t)emuenv_.kernel.threads.size());
		out.push_back(std::move(s));
	}
	bool supports_state() const override { return false; }
	bool save_state(std::vector<uint8_t>&, std::string& error) override { error = "Vita3K has no savestates"; return false; }
	bool load_state(const std::vector<uint8_t>&, std::string& error) override { error = "Vita3K has no savestates"; return false; }
	bool capture(FrameImage& image, std::string& error) override {
		const std::lock_guard<std::mutex> lock(g_frame_mutex);
		if (g_frame.rgb.empty()) { error = "no frame has been presented yet"; return false; }
		image = g_frame;
		return true;
	}
	bool controlled_start() const override { return false; }
	void halt() override { emuenv_.kernel.pause_threads(); }
	void unhalt() override { emuenv_.kernel.resume_threads(); }
	std::vector<std::string> active_exceptions() const override {
		return {"vita3k.execution.instruction-step-absent", "vita3k.input-hold.port-zero-only"};
	}
	void extend_hello(Json& hello) const override {
		Json notes = Json::object();
		notes.set("frames", Json("step counts emulated vblanks; guest threads run on host threads between them, so frame contents are not repeatable"));
		notes.set("frozen", Json("a frozen stop parks the vblank thread and pauses every guest kernel thread"));
		notes.set("savestates", Json("Vita3K has no savestates"));
		notes.set("screenshot", Json("last presented OpenGL frame (960x544 window)"));
		hello.set("capability_notes", notes);
	}

private:
	EmuEnvState& emuenv_;
	std::string content_;
	std::vector<Region> regions_;
};

std::unique_ptr<VitaBackend> g_backend;
std::unique_ptr<emucap_producer::Producer> g_producer;

}  // namespace

void emucap_vita3k_start(EmuEnvState& emuenv) {
	if (g_producer) return;
	const char* content = std::getenv("EMUCAP_CONTENT");
	g_backend = std::make_unique<VitaBackend>(emuenv, content ? content : "");
	g_producer = std::make_unique<emucap_producer::Producer>(*g_backend);
	if (!g_producer->start()) {
		g_producer.reset();
		g_backend.reset();
		return;
	}
	g_emucap_vblank_hook = emucap_vita3k_vblank;
}

void emucap_vita3k_stop() {
	g_emucap_vblank_hook = nullptr;
	if (g_producer) g_producer->stop();
}

void emucap_vita3k_vblank() {
	if (g_producer) g_producer->boundary();
}

void emucap_vita3k_capture(SDL_Window* window) {
	if (!g_producer || !window) return;
	static ReadPixels read_pixels = reinterpret_cast<ReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));
	static PixelStore pixel_store = reinterpret_cast<PixelStore>(SDL_GL_GetProcAddress("glPixelStorei"));
	static ReadBuffer read_buffer = reinterpret_cast<ReadBuffer>(SDL_GL_GetProcAddress("glReadBuffer"));
	if (!read_pixels || !pixel_store) return;
	int w = 0, h = 0;
	if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w <= 0 || h <= 0) return;
	std::vector<uint8_t> flipped((size_t)w * h * 3);
	if (read_buffer) read_buffer(0x0405 /* GL_BACK */);
	pixel_store(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
	read_pixels(0, 0, w, h, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, flipped.data());
	// Swaps without a new guest frame (pause overlay ticks) present an empty back buffer; keep
	// the last frame that has content. Vita3K's screen renderer already flips the image, so the
	// readback rows are top-down.
	bool any = false;
	for (size_t i = 0; i < flipped.size() && !any; i += 97) any = flipped[i] != 0;
	if (!any) return;
	const std::lock_guard<std::mutex> lock(g_frame_mutex);
	g_frame.width = w;
	g_frame.height = h;
	g_frame.rgb.swap(flipped);
	g_presented++;
}
