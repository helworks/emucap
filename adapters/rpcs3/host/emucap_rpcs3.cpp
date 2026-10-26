// emucap producer integration for RPCS3 (PlayStation 3).
//
// build.sh copies this file into the RPCS3 work tree (rpcs3/Emu) and applies the hook patch. The
// producer is self-paced: requests run on its own service thread, a frozen stop is Emu.Pause(),
// and step(n) uses RPCS3's pause-after-N-flips frame advance, so the guest stops at exactly the
// n-th emulated flip. The RSX flip hook counts flips and keeps the displayed buffer, read back from
// guest local memory (requires "Write Color Buffers"). cellPad port 0 reports the held buttons.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "stdafx.h"
#include "emucap_rpcs3.h"

#include "emucap_producer.h"

#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/lv2/sys_rsx.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#if __has_include("emucap_build_id.h")
#include "emucap_build_id.h"
#endif
#ifndef EMUCAP_BUILD_ID
#define EMUCAP_BUILD_ID "unknown"
#endif

std::atomic<bool> g_emucap_pad_active{false};
std::atomic<uint32_t> g_emucap_pad_digital1{0};
std::atomic<uint32_t> g_emucap_pad_digital2{0};

namespace {

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Json;
using emucap_producer::Region;

struct Button { const char* name; uint32_t d1; uint32_t d2; };
// cellPad digital masks (Emu/Io/pad_types.h).
const Button kButtons[] = {
	{"cross", 0, 0x40}, {"circle", 0, 0x20}, {"square", 0, 0x80}, {"triangle", 0, 0x10},
	{"l1", 0, 0x04}, {"r1", 0, 0x08}, {"l2", 0, 0x01}, {"r2", 0, 0x02},
	{"select", 0x01, 0}, {"start", 0x08, 0}, {"l3", 0x02, 0}, {"r3", 0x04, 0},
	{"up", 0x10, 0}, {"right", 0x20, 0}, {"down", 0x40, 0}, {"left", 0x80, 0},
};

std::atomic<uint64_t> g_flips{0};
std::mutex g_frame_mutex;
FrameImage g_frame;

bool wait_until(const std::function<bool()>& done, int64_t deadline_ms) {
	while (!done()) {
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now >= deadline_ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return true;
}

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Rpcs3Backend : public emucap_producer::Backend {
public:
	explicit Rpcs3Backend(std::string content) : content_(std::move(content)) {
		auto read = [](uint64_t a, uint8_t* out, size_t n) {
			if (n == 0 || a + n > (1ull << 32)) return false;
			if (!vm::check_addr((u32)a, vm::page_readable, (u32)n)) return false;
			std::memcpy(out, vm::g_base_addr + a, n);
			return true;
		};
		auto write = [](uint64_t a, const uint8_t* in, size_t n) {
			if (n == 0 || a + n > (1ull << 32)) return false;
			if (!vm::check_addr((u32)a, vm::page_writable, (u32)n)) return false;
			std::memcpy(vm::g_base_addr + a, in, n);
			return true;
		};
		// The 32-bit guest virtual address space (main memory 0x00010000.., user memory
		// 0x20000000.., RSX local memory 0xC0000000..). Unmapped pages fail the access.
		regions_.push_back({"vm", 1ull << 32, true, false, read, write});
		// Main memory as an offset-based window for batch reads.
		regions_.push_back({"main", 0x10000000, true, true, read, write});
	}

	std::string system() const override { return "ps3"; }
	std::string adapter() const override { return "rpcs3"; }
	std::string build() const override { return EMUCAP_BUILD_ID; }
	std::string content_path() const override { return content_; }
	std::vector<Region>& regions() override { return regions_; }
	std::vector<std::string> button_names() const override {
		std::vector<std::string> names;
		for (const auto& b : kButtons) names.push_back(b.name);
		return names;
	}
	void set_buttons(uint64_t mask) override {
		uint32_t d1 = 0, d2 = 0;
		for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
			if (mask & (1ull << i)) {
				d1 |= kButtons[i].d1;
				d2 |= kButtons[i].d2;
			}
		}
		g_emucap_pad_digital1 = d1;
		g_emucap_pad_digital2 = d2;
	}
	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		CpuState s;
		s.cpu_id = "ppu0";
		s.aliases = {"main", "cpu", "ppu"};
		bool found = false;
		idm::select<named_thread<ppu_thread>>([&](u32 id, ppu_thread& ppu) {
			if (found) return;
			found = true;
			for (int i = 0; i < 32; i++) s.registers.emplace_back("cpu.r" + std::to_string(i), ppu.gpr[i]);
			s.registers.emplace_back("cpu.pc", ppu.cia);
			s.registers.emplace_back("cpu.lr", ppu.lr);
			s.registers.emplace_back("cpu.ctr", ppu.ctr);
			s.registers.emplace_back("thread.id", id);
		});
		out.push_back(std::move(s));
	}
	bool supports_state() const override { return false; }
	bool save_state(std::vector<uint8_t>&, std::string& error) override { error = "RPCS3 savestates are not wired to emucap"; return false; }
	bool load_state(const std::vector<uint8_t>&, std::string& error) override { error = "RPCS3 savestates are not wired to emucap"; return false; }
	bool capture(FrameImage& image, std::string& error) override {
		const std::lock_guard<std::mutex> lock(g_frame_mutex);
		if (g_frame.rgb.empty()) { error = "no frame has been flipped yet"; return false; }
		image = g_frame;
		return true;
	}
	bool controlled_start() const override { return false; }
	bool self_paced() const override { return true; }
	uint64_t frame_count() const override { return g_flips.load(); }
	void halt() override {
		// The guest boots (and compiles PPU code) after the producer connects; a frozen stop
		// waits for it to run first.
		wait_until([] { return Emu.IsRunning() || Emu.IsPaused(); }, now_ms() + 240000);
		if (!Emu.IsRunning()) return;
		Emu.Pause(false, false);
		wait_until([] { return Emu.IsPaused() || Emu.IsStopped(); }, now_ms() + 10000);
	}
	void unhalt() override {
		if (Emu.IsPaused()) Emu.Resume();
	}
	bool advance(uint64_t frames, int64_t deadline_ms, uint64_t& completed, std::string& error) override {
		completed = 0;
		auto* rsx = rsx::get_current_renderer();
		if (rsx && !Emu.IsPaused()) halt();
		if (!rsx || !Emu.IsPaused()) { error = "the guest is not booted and paused"; return false; }
		const uint64_t before = g_flips.load();
		rsx->emucap_pause_after_flips((u32)frames);
		Emu.Resume();
		const bool ok = wait_until([] { return Emu.IsPaused() || Emu.IsStopped(); }, deadline_ms);
		if (!ok) halt();
		completed = g_flips.load() - before;
		if (!ok) error = "host_deadline";
		return ok;
	}
	std::vector<std::string> active_exceptions() const override {
		return {"rpcs3.execution.instruction-step-absent", "rpcs3.input-hold.port-zero-only"};
	}
	void extend_hello(Json& hello) const override {
		Json notes = Json::object();
		notes.set("frames", Json("step counts emulated flips; RPCS3 pauses itself at the n-th flip"));
		notes.set("frozen", Json("a frozen stop is Emu.Pause(); PPU, SPU and RSX threads are suspended"));
		notes.set("screenshot", Json("the flipped display buffer read from RSX local memory (requires Write Color Buffers)"));
		notes.set("savestates", Json("not wired: RPCS3 savestates are boot-time containers, not in-process snapshots"));
		hello.set("capability_notes", notes);
	}

private:
	std::string content_;
	std::vector<Region> regions_;
};

std::unique_ptr<Rpcs3Backend> g_backend;
std::unique_ptr<emucap_producer::Producer> g_producer;

}  // namespace

void emucap_rpcs3_start(const std::string& content) {
	if (g_producer) return;
	const char* env = std::getenv("EMUCAP_CONTENT");
	g_backend = std::make_unique<Rpcs3Backend>(env && env[0] ? std::string(env) : content);
	g_producer = std::make_unique<emucap_producer::Producer>(*g_backend);
	if (!g_producer->start()) {
		g_producer.reset();
		g_backend.reset();
		return;
	}
	g_emucap_pad_active = true;
}

void emucap_rpcs3_on_flip(rsx::thread* rsx, u32 buffer) {
	g_flips++;
	if (!g_producer || !rsx || buffer >= rsx->display_buffers_count) return;
	const RsxDisplayInfo& info = rsx->display_buffers[buffer];
	const u32 width = info.width, height = info.height, pitch = info.pitch;
	if (!width || !height || pitch < width * 4 || width > 4096 || height > 4096) return;
	const u32 address = rsx::get_address(info.offset, CELL_GCM_LOCATION_LOCAL);
	if (!address || !vm::check_addr(address, vm::page_readable, pitch * height)) return;
	const u8* src = vm::g_base_addr + address;
	const std::lock_guard<std::mutex> lock(g_frame_mutex);
	g_frame.width = (int)width;
	g_frame.height = (int)height;
	g_frame.rgb.resize((size_t)width * height * 3);
	for (u32 y = 0; y < height; y++) {
		const u8* row = src + (size_t)pitch * y;
		u8* out = &g_frame.rgb[(size_t)y * width * 3];
		for (u32 x = 0; x < width; x++) {
			// A8R8G8B8, big-endian in guest memory: A R G B.
			out[3 * x] = row[4 * x + 1];
			out[3 * x + 1] = row[4 * x + 2];
			out[3 * x + 2] = row[4 * x + 3];
		}
	}
}
