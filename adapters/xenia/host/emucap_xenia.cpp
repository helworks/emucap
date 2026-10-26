// emucap producer integration for Xenia Canary (Xbox 360).
//
// build.sh copies this file into the Xenia work tree (src/xenia/app/emucap) and applies the hook
// edits: the command processor calls the swap hook after every XE_SWAP packet (the guest VdSwap,
// right after IssueSwap refreshed the guest output image), and the input system asks the pad hook
// for user 0 before it consults its drivers. A frozen stop parks the command processor thread at
// the swap and suspends every guest thread the way Xenia's debugger pause does.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "emucap_xenia.h"

#include "emucap_producer.h"

#include "xenia/apu/audio_system.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if __has_include("emucap_build_id.h")
#include "emucap_build_id.h"
#endif
#ifndef EMUCAP_BUILD_ID
#define EMUCAP_BUILD_ID "unknown"
#endif

namespace xe {
namespace gpu {
extern void (*g_emucap_swap_hook)();
}  // namespace gpu
namespace hid {
extern bool (*g_emucap_pad_hook)(uint32_t user_index, uint16_t* buttons);
}  // namespace hid
}  // namespace xe

namespace {

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Json;
using emucap_producer::Region;

struct Button { const char* name; uint16_t bit; };
// XINPUT_GAMEPAD button bits (hid/input.h).
const Button kButtons[] = {
	{"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000},
	{"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
	{"start", 0x0010}, {"back", 0x0020}, {"lb", 0x0100}, {"rb", 0x0200},
	{"ls", 0x0040}, {"rs", 0x0080}, {"guide", 0x0400},
};

std::atomic<uint16_t> g_pad_buttons{0};
std::atomic<bool> g_pad_engaged{false};

xe::Emulator* emulator() {
	auto* kernel = xe::kernel::kernel_state();
	return kernel ? kernel->emulator() : nullptr;
}

constexpr uint64_t kPhysicalSize = 512ull << 20;
constexpr uint32_t kPageSize = 4096;

bool virtual_range_readable(xe::Memory* memory, uint64_t address, size_t length, bool write) {
	if (length == 0 || address + length > (1ull << 32)) return false;
	const uint64_t first = address & ~uint64_t(kPageSize - 1);
	for (uint64_t page = first; page < address + length; page += kPageSize) {
		auto* heap = memory->LookupHeap((uint32_t)page);
		if (!heap) return false;
		uint32_t protect = 0;
		if (!heap->QueryProtect((uint32_t)page, &protect)) return false;
		if (!(protect & xe::kMemoryProtectRead)) return false;
		if (write && !(protect & xe::kMemoryProtectWrite)) return false;
	}
	return true;
}

class XeniaBackend : public emucap_producer::Backend {
public:
	explicit XeniaBackend(std::string content) : content_(std::move(content)) {
		// Guest physical memory (512 MiB, big-endian contents as the guest sees them). Xenia maps
		// it in full, so every offset is readable without touching guest page state.
		regions_.push_back({"main", kPhysicalSize, true, true,
			[](uint64_t a, uint8_t* out, size_t n) {
				auto* emu = emulator();
				if (!emu || n == 0 || a + n > kPhysicalSize) return false;
				std::memcpy(out, emu->memory()->TranslatePhysical((uint32_t)a), n);
				return true;
			},
			[](uint64_t a, const uint8_t* in, size_t n) {
				auto* emu = emulator();
				if (!emu || n == 0 || a + n > kPhysicalSize) return false;
				std::memcpy(emu->memory()->TranslatePhysical((uint32_t)a), in, n);
				return true;
			}});
		// The title's 32-bit virtual address space; reads and writes need committed pages with
		// the matching protection.
		regions_.push_back({"vm", 1ull << 32, true, false,
			[](uint64_t a, uint8_t* out, size_t n) {
				auto* emu = emulator();
				if (!emu || !virtual_range_readable(emu->memory(), a, n, false)) return false;
				std::memcpy(out, emu->memory()->TranslateVirtual((uint32_t)a), n);
				return true;
			},
			[](uint64_t a, const uint8_t* in, size_t n) {
				auto* emu = emulator();
				if (!emu || !virtual_range_readable(emu->memory(), a, n, true)) return false;
				std::memcpy(emu->memory()->TranslateVirtual((uint32_t)a), in, n);
				return true;
			}});
	}

	std::string system() const override { return "x360"; }
	std::string adapter() const override { return "xenia"; }
	std::string build() const override { return EMUCAP_BUILD_ID; }
	std::string content_path() const override { return content_; }
	std::vector<Region>& regions() override { return regions_; }
	std::vector<std::string> button_names() const override {
		std::vector<std::string> names;
		for (const auto& b : kButtons) names.push_back(b.name);
		return names;
	}
	void set_buttons(uint64_t mask) override {
		uint16_t bits = 0;
		for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
			if (mask & (1ull << i)) bits |= kButtons[i].bit;
		g_pad_buttons.store(bits);
	}
	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		CpuState s;
		s.cpu_id = "main_thread";
		s.aliases = {"main", "cpu", "ppu"};
		auto* kernel = xe::kernel::kernel_state();
		if (kernel) {
			auto threads = kernel->object_table()->GetObjectsByType<xe::kernel::XThread>(
				xe::kernel::XObject::Type::Thread);
			uint64_t guest_threads = 0;
			for (auto& thread : threads) {
				if (!thread->is_guest_thread()) continue;
				guest_threads++;
				if (!thread->main_thread() || !thread->thread_state()) continue;
				const auto* ctx = thread->thread_state()->context();
				if (!ctx) continue;
				for (int i = 0; i < 32; i++) s.registers.emplace_back("cpu.r" + std::to_string(i), ctx->r[i]);
				s.registers.emplace_back("cpu.lr", ctx->lr);
				s.registers.emplace_back("cpu.ctr", ctx->ctr);
				s.registers.emplace_back("thread.id", (uint64_t)thread->thread_id());
			}
			s.registers.emplace_back("kernel.guest_threads", guest_threads);
		}
		out.push_back(std::move(s));
	}
	bool supports_state() const override { return false; }
	bool save_state(std::vector<uint8_t>&, std::string& error) override {
		error = "Xenia savestates are not available on Linux: saving guest threads needs the stack walker, which is unimplemented on POSIX";
		return false;
	}
	bool load_state(const std::vector<uint8_t>&, std::string& error) override {
		error = "Xenia savestates are not available on Linux";
		return false;
	}
	bool capture(FrameImage& image, std::string& error) override {
		auto* emu = emulator();
		auto* graphics = emu ? emu->graphics_system() : nullptr;
		auto* presenter = graphics ? graphics->presenter() : nullptr;
		if (!presenter) { error = "Xenia has no presenter (no display window)"; return false; }
		xe::ui::RawImage raw;
		if (!presenter->CaptureGuestOutput(raw) || raw.width == 0 || raw.height == 0) {
			error = "no guest frame has been presented yet";
			return false;
		}
		image.width = (int)raw.width;
		image.height = (int)raw.height;
		image.rgb.resize((size_t)raw.width * raw.height * 3);
		for (uint32_t y = 0; y < raw.height; y++) {
			const uint8_t* src = raw.data.data() + raw.stride * y;
			uint8_t* dst = &image.rgb[(size_t)y * raw.width * 3];
			for (uint32_t x = 0; x < raw.width; x++) {
				dst[x * 3 + 0] = src[x * 4 + 0];
				dst[x * 3 + 1] = src[x * 4 + 1];
				dst[x * 3 + 2] = src[x * 4 + 2];
			}
		}
		return true;
	}
	bool controlled_start() const override { return false; }
	// Stop every guest thread the way Emulator::Pause() does, without pausing the command
	// processor (the calling thread, parked at the swap) or the audio system (its Pause() waits
	// for the audio worker, which can itself be waiting on a guest thread). Audio worker threads
	// that run guest callbacks are guest-suspendable threads and stop with the rest.
	void halt() override {
		auto* emu = emulator();
		if (!emu || !suspended_.empty()) return;
		auto* kernel = emu->kernel_state();
		// Hold the global critical region so no thread is suspended inside it; a guest thread can
		// hold it briefly, so retry instead of blocking the command processor indefinitely.
		auto lock = xe::global_critical_region::TryAcquire();
		for (int i = 0; i < 2000 && !lock.owns_lock(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			lock = xe::global_critical_region::TryAcquire();
		}
		if (!lock.owns_lock()) {
			std::fprintf(stderr, "emucap-xenia: global critical region busy; frozen stop without guest thread suspension\n");
			return;
		}
		auto threads = kernel->object_table()->GetObjectsByType<xe::kernel::XThread>(
			xe::kernel::XObject::Type::Thread);
		auto* current = xe::kernel::XThread::IsInThread() ? xe::kernel::XThread::GetCurrentThread() : nullptr;
		for (auto& thread : threads) {
			if (thread.get() == current || !thread->can_debugger_suspend() || !thread->is_running()) continue;
			if (thread->thread() && thread->thread()->Suspend(nullptr)) suspended_.push_back(thread);
		}
	}
	void unhalt() override {
		for (auto& thread : suspended_)
			if (thread->thread()) thread->thread()->Resume(nullptr);
		suspended_.clear();
	}
	std::vector<std::string> active_exceptions() const override {
		return {"xenia.execution.instruction-step-absent", "xenia.input-hold.port-zero-only"};
	}
	void extend_hello(Json& hello) const override {
		Json notes = Json::object();
		notes.set("frames", Json("step counts guest VdSwap packets executed by the command processor; guest threads run on host threads between them, so frame contents are not repeatable"));
		notes.set("frozen", Json("a frozen stop parks the command processor at the swap and suspends every guest thread (Xenia's debugger pause)"));
		notes.set("savestates", Json("unavailable: Xenia's experimental savestates step guest threads to safe points with the stack walker, which is unimplemented on POSIX"));
		notes.set("screenshot", Json("guest output image of the last swap (Vulkan presenter capture)"));
		notes.set("input", Json("XInput gamepad, user 0; Xenia's own input drivers are bypassed for user 0"));
		notes.set("memory", Json("main = guest physical memory (512 MiB), vm = title virtual address space"));
		hello.set("capability_notes", notes);
	}

private:
	std::string content_;
	std::vector<Region> regions_;
	std::vector<xe::kernel::object_ref<xe::kernel::XThread>> suspended_;
};

std::unique_ptr<XeniaBackend> g_backend;
std::unique_ptr<emucap_producer::Producer> g_producer;

void swap_hook() {
	if (g_producer) g_producer->boundary();
}

bool pad_hook(uint32_t user_index, uint16_t* buttons) {
	if (!g_pad_engaged.load() || user_index != 0) return false;
	*buttons = g_pad_buttons.load();
	return true;
}

// Connect at process start so launch readiness does not wait for the first swap; requests other
// than hello/status/get_rom_info wait for the first frame boundary.
struct AutoStart {
	AutoStart() {
		if (!std::getenv("EMUCAP_PORT")) return;
		const char* content = std::getenv("EMUCAP_CONTENT");
		g_backend = std::make_unique<XeniaBackend>(content ? content : "");
		g_producer = std::make_unique<emucap_producer::Producer>(*g_backend);
		if (!g_producer->start()) {
			g_producer.reset();
			g_backend.reset();
			return;
		}
		g_pad_engaged = true;
		xe::gpu::g_emucap_swap_hook = swap_hook;
		xe::hid::g_emucap_pad_hook = pad_hook;
	}
} g_autostart;

}  // namespace

bool emucap_xenia_active() { return g_producer != nullptr; }
