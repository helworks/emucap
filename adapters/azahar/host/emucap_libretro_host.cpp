// emucap-azahar-host: headless libretro frontend for the Azahar (Nintendo 3DS) core.
//
// Loads azahar_libretro.so, boots the content with the software renderer, and serves emucap's
// control wire through adapters/_common/emucap_producer. One retro_run() is one emulated frame;
// requests run between retro_run() calls on this (the only) emulator thread.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro.h"

#include "emucap_producer.h"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef EMUCAP_BUILD_ID
#define EMUCAP_BUILD_ID "unknown"
#endif

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Json;
using emucap_producer::Region;

namespace {

struct Core {
	void* handle = nullptr;
	void (*set_environment)(retro_environment_t) = nullptr;
	void (*set_video_refresh)(retro_video_refresh_t) = nullptr;
	void (*set_audio_sample)(retro_audio_sample_t) = nullptr;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
	void (*set_input_poll)(retro_input_poll_t) = nullptr;
	void (*set_input_state)(retro_input_state_t) = nullptr;
	void (*init)() = nullptr;
	void (*deinit)() = nullptr;
	void (*get_system_info)(retro_system_info*) = nullptr;
	void (*get_system_av_info)(retro_system_av_info*) = nullptr;
	void (*set_controller_port_device)(unsigned, unsigned) = nullptr;
	void (*reset)() = nullptr;
	void (*run)() = nullptr;
	size_t (*serialize_size)() = nullptr;
	bool (*serialize)(void*, size_t) = nullptr;
	bool (*unserialize)(const void*, size_t) = nullptr;
	bool (*load_game)(const retro_game_info*) = nullptr;
	void (*unload_game)() = nullptr;
};

// Optional debug exports from the emucap-patched Azahar core (patches/0002).
struct Exports {
	size_t (*fcram_size)() = nullptr;
	uint8_t* (*fcram_pointer)() = nullptr;
	size_t (*read_vaddr)(uint32_t, void*, size_t) = nullptr;
	unsigned (*cpu_count)() = nullptr;
	bool (*cpu_registers)(unsigned, uint32_t*) = nullptr;
};

struct State {
	Core core;
	Exports exports;
	std::string home;
	std::string content;
	std::map<std::string, std::string> options;
	retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
	FrameImage frame;
	bool frame_fresh = false;
	uint16_t buttons = 0;  // libretro joypad id bits
	bool pointer_down = false;
	int pointer_x = 0, pointer_y = 0;
	retro_frame_time_callback frame_time{};
	bool has_frame_time = false;
	retro_audio_callback audio_cb{};
	bool has_audio_cb = false;
	std::vector<retro_memory_descriptor> memory;
	bool shutdown = false;
	int log_level = RETRO_LOG_WARN;
};

State g;

void log_cb(enum retro_log_level level, const char* fmt, ...) {
	if ((int)level < g.log_level) return;
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
}

bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		static_cast<retro_log_callback*>(data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*static_cast<const char**>(data) = g.home.c_str();
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*static_cast<unsigned*>(data) = 0;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		auto* var = static_cast<retro_variable*>(data);
		auto it = g.options.find(var->key ? var->key : "");
		var->value = it == g.options.end() ? nullptr : it->second.c_str();
		return it != g.options.end();
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*static_cast<bool*>(data) = false;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		g.pixel_format = *static_cast<const retro_pixel_format*>(data);
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
		*static_cast<bool*>(data) = true;
		return true;
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		*static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_NONE;
		return true;
	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
		g.frame_time = *static_cast<const retro_frame_time_callback*>(data);
		g.has_frame_time = true;
		return true;
	case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
		g.audio_cb = *static_cast<const retro_audio_callback*>(data);
		g.has_audio_cb = true;
		return true;
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
		const auto* map = static_cast<const retro_memory_map*>(data);
		g.memory.assign(map->descriptors, map->descriptors + map->num_descriptors);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: {
		const auto* msg = static_cast<const retro_message*>(data);
		if (msg && msg->msg) std::fprintf(stderr, "[core] %s\n", msg->msg);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		return true;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		g.shutdown = true;
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*static_cast<int*>(data) = 3;
		return true;
	default:
		// SET_HW_RENDER, VFS, sensors, microphone, shared contexts: not offered (software only).
		return false;
	}
}

void video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data || data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0) return;
	g.frame.width = (int)width;
	g.frame.height = (int)height;
	g.frame.rgb.resize((size_t)width * height * 3);
	const auto* base = static_cast<const uint8_t*>(data);
	for (unsigned y = 0; y < height; y++) {
		const uint8_t* row = base + pitch * y;
		uint8_t* out = &g.frame.rgb[(size_t)y * width * 3];
		for (unsigned x = 0; x < width; x++) {
			uint8_t r, gch, b;
			if (g.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t px;
				std::memcpy(&px, row + x * 4, 4);
				r = (uint8_t)(px >> 16); gch = (uint8_t)(px >> 8); b = (uint8_t)px;
			} else {
				uint16_t px;
				std::memcpy(&px, row + x * 2, 2);
				if (g.pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
					r = (uint8_t)(((px >> 11) & 31) * 255 / 31); gch = (uint8_t)(((px >> 5) & 63) * 255 / 63); b = (uint8_t)((px & 31) * 255 / 31);
				} else {
					r = (uint8_t)(((px >> 10) & 31) * 255 / 31); gch = (uint8_t)(((px >> 5) & 31) * 255 / 31); b = (uint8_t)((px & 31) * 255 / 31);
				}
			}
			out[3 * x] = r; out[3 * x + 1] = gch; out[3 * x + 2] = b;
		}
	}
	g.frame_fresh = true;
}

void audio_sample(int16_t, int16_t) {}
size_t audio_sample_batch(const int16_t*, size_t frames) { return frames; }
void input_poll() {}

int16_t input_state(unsigned port, unsigned device, unsigned, unsigned id) {
	if (port != 0) return 0;
	if (device == RETRO_DEVICE_JOYPAD) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)g.buttons;
		return (g.buttons >> id) & 1;
	}
	if (device == RETRO_DEVICE_POINTER) {
		const int w = g.frame.width > 0 ? g.frame.width : 400;
		const int h = g.frame.height > 0 ? g.frame.height : 480;
		switch (id) {
		case RETRO_DEVICE_ID_POINTER_X: return (int16_t)((g.pointer_x * 2 * 0x7fff) / w - 0x7fff);
		case RETRO_DEVICE_ID_POINTER_Y: return (int16_t)((g.pointer_y * 2 * 0x7fff) / h - 0x7fff);
		case RETRO_DEVICE_ID_POINTER_PRESSED: return g.pointer_down ? 1 : 0;
		case RETRO_DEVICE_ID_POINTER_COUNT: return g.pointer_down ? 1 : 0;
		default: return 0;
		}
	}
	return 0;
}

template <typename T>
bool sym(void* handle, const char* name, T& out) {
	out = reinterpret_cast<T>(dlsym(handle, name));
	if (!out) std::fprintf(stderr, "[emucap] core lacks %s\n", name);
	return out != nullptr;
}

bool load_core(const std::string& path) {
	Core& c = g.core;
	c.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!c.handle) { std::fprintf(stderr, "[emucap] dlopen %s: %s\n", path.c_str(), dlerror()); return false; }
	return sym(c.handle, "retro_set_environment", c.set_environment)
		&& sym(c.handle, "retro_set_video_refresh", c.set_video_refresh)
		&& sym(c.handle, "retro_set_audio_sample", c.set_audio_sample)
		&& sym(c.handle, "retro_set_audio_sample_batch", c.set_audio_sample_batch)
		&& sym(c.handle, "retro_set_input_poll", c.set_input_poll)
		&& sym(c.handle, "retro_set_input_state", c.set_input_state)
		&& sym(c.handle, "retro_init", c.init)
		&& sym(c.handle, "retro_deinit", c.deinit)
		&& sym(c.handle, "retro_get_system_info", c.get_system_info)
		&& sym(c.handle, "retro_get_system_av_info", c.get_system_av_info)
		&& sym(c.handle, "retro_set_controller_port_device", c.set_controller_port_device)
		&& sym(c.handle, "retro_reset", c.reset)
		&& sym(c.handle, "retro_run", c.run)
		&& sym(c.handle, "retro_serialize_size", c.serialize_size)
		&& sym(c.handle, "retro_serialize", c.serialize)
		&& sym(c.handle, "retro_unserialize", c.unserialize)
		&& sym(c.handle, "retro_load_game", c.load_game)
		&& sym(c.handle, "retro_unload_game", c.unload_game);
}

void load_exports() {
	Exports& e = g.exports;
	e.fcram_size = reinterpret_cast<size_t (*)()>(dlsym(g.core.handle, "emucap_fcram_size"));
	e.fcram_pointer = reinterpret_cast<uint8_t* (*)()>(dlsym(g.core.handle, "emucap_fcram_pointer"));
	e.read_vaddr = reinterpret_cast<size_t (*)(uint32_t, void*, size_t)>(dlsym(g.core.handle, "emucap_read_vaddr"));
	e.cpu_count = reinterpret_cast<unsigned (*)()>(dlsym(g.core.handle, "emucap_cpu_count"));
	e.cpu_registers = reinterpret_cast<bool (*)(unsigned, uint32_t*)>(dlsym(g.core.handle, "emucap_cpu_registers"));
	if (!e.fcram_pointer || !e.cpu_registers)
		std::fprintf(stderr, "[emucap] core lacks the emucap debug exports; memory and registers are limited\n");
}

// 3DS controls exposed by the Azahar core (libretro joypad ids). L3 is HOME and also toggles the
// screen layout in the libretro core, so it is not offered; R3 is a touch-press alias.
struct Button { const char* name; unsigned id; };
const Button kButtons[] = {
	{"a", RETRO_DEVICE_ID_JOYPAD_A}, {"b", RETRO_DEVICE_ID_JOYPAD_B},
	{"x", RETRO_DEVICE_ID_JOYPAD_X}, {"y", RETRO_DEVICE_ID_JOYPAD_Y},
	{"l", RETRO_DEVICE_ID_JOYPAD_L}, {"r", RETRO_DEVICE_ID_JOYPAD_R},
	{"zl", RETRO_DEVICE_ID_JOYPAD_L2}, {"zr", RETRO_DEVICE_ID_JOYPAD_R2},
	{"start", RETRO_DEVICE_ID_JOYPAD_START}, {"select", RETRO_DEVICE_ID_JOYPAD_SELECT},
	{"up", RETRO_DEVICE_ID_JOYPAD_UP}, {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
	{"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
};

const char* region_name(uint64_t start) {
	if (start >= 0x08000000 && start < 0x10000000) return "heap";
	if (start >= 0x14000000 && start < 0x1C000000) return "linear_heap";
	if (start >= 0x30000000 && start < 0x40000000) return "new_linear_heap";
	if (start >= 0x1F000000 && start < 0x1F600000) return "vram";
	return nullptr;
}

class AzaharBackend : public emucap_producer::Backend {
public:
	void build_regions() {
		regions_.clear();
		const Exports& e = g.exports;
		if (e.fcram_size && e.fcram_pointer && e.fcram_size() > 0) {
			const uint64_t size = e.fcram_size();
			// Physical FCRAM (application, system and base regions); the backing array is fixed
			// for the life of the core, including across retro_unserialize.
			regions_.push_back({"fcram", size, true, true,
				[size](uint64_t a, uint8_t* out, size_t n) {
					uint8_t* p = g.exports.fcram_pointer();
					if (!p || a > size || n > size - a) return false;
					std::memcpy(out, p + a, n);
					return true;
				},
				[size](uint64_t a, const uint8_t* in, size_t n) {
					uint8_t* p = g.exports.fcram_pointer();
					if (!p || a > size || n > size - a) return false;
					std::memcpy(p + a, in, n);
					return true;
				}});
		}
		if (e.read_vaddr) {
			// The current process' virtual address space; unmapped addresses fail the read.
			regions_.push_back({"vaddr", 1ull << 32, false, false,
				[](uint64_t a, uint8_t* out, size_t n) { return g.exports.read_vaddr((uint32_t)a, out, n) == n; },
				nullptr});
		}
		std::map<std::string, int> seen;
		for (const auto& d : g.memory) {
			const char* base = region_name(d.start);
			if (!base || !d.ptr || d.len == 0) continue;
			// The heap VMA published at load time is the tiny pre-main heap; physical FCRAM and
			// vaddr cover application memory instead.
			if (std::string(base) != "vram") continue;
			std::string name = base;
			if (seen[name]++) name += "_" + std::to_string(seen[base] - 1);
			const uint64_t start = d.start, size = d.len;
			// Resolve the descriptor at access time: a loaded state may remap backing memory.
			auto find = [start]() -> uint8_t* {
				for (const auto& m : g.memory)
					if (m.start == start && m.ptr) return static_cast<uint8_t*>(m.ptr) + m.offset;
				return nullptr;
			};
			regions_.push_back({name, size, true, true,
				[find, size](uint64_t a, uint8_t* out, size_t n) {
					uint8_t* p = find();
					if (!p || a > size || n > size - a) return false;
					std::memcpy(out, p + a, n);
					return true;
				},
				[find, size](uint64_t a, const uint8_t* in, size_t n) {
					uint8_t* p = find();
					if (!p || a > size || n > size - a) return false;
					std::memcpy(p + a, in, n);
					return true;
				}});
			bases_.push_back({name, start});
		}
	}

	std::string system() const override { return "3ds"; }
	std::string adapter() const override { return "azahar"; }
	std::string build() const override { return EMUCAP_BUILD_ID; }
	std::string content_path() const override { return g.content; }
	std::vector<Region>& regions() override { return regions_; }
	std::vector<std::string> button_names() const override {
		std::vector<std::string> names;
		for (const auto& b : kButtons) names.push_back(b.name);
		return names;
	}
	void set_buttons(uint64_t mask) override {
		uint16_t bits = 0;
		for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++)
			if (mask & (1ull << i)) bits |= (uint16_t)(1u << kButtons[i].id);
		g.buttons = bits;
	}
	bool has_pointer() const override { return true; }
	bool set_pointer(bool down, int x, int y) override {
		g.pointer_down = down;
		g.pointer_x = x;
		g.pointer_y = y;
		return true;
	}
	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		const Exports& e = g.exports;
		const unsigned cores = e.cpu_count && e.cpu_registers ? std::max(1u, e.cpu_count()) : 1u;
		for (unsigned core = 0; core < cores; core++) {
			CpuState s;
			s.cpu_id = core == 0 ? "arm11" : "arm11_core" + std::to_string(core);
			if (core == 0) s.aliases = {"main", "cpu", "appcore", "arm11_core0"};
			uint32_t regs[17] = {};
			if (e.cpu_registers && e.cpu_registers(core, regs)) {
				for (int i = 0; i < 15; i++) s.registers.emplace_back("cpu.r" + std::to_string(i), regs[i]);
				s.registers.emplace_back("cpu.pc", regs[15]);
				s.registers.emplace_back("cpu.cpsr", regs[16]);
			}
			out.push_back(std::move(s));
		}
	}
	bool save_state(std::vector<uint8_t>& out, std::string& error) override {
		size_t size = g.core.serialize_size();
		if (size == 0) { error = "the core cannot serialize now (async operations pending or not powered on)"; return false; }
		out.resize(size);
		if (!g.core.serialize(out.data(), out.size())) { error = "retro_serialize failed"; return false; }
		for (int i = 0; i < 2; i++) out.push_back((uint8_t)(g.buttons >> (8 * i)));
		return true;
	}
	bool load_state(const std::vector<uint8_t>& data, std::string& error) override {
		if (data.size() < 2) { error = "savestate payload is truncated"; return false; }
		if (!g.core.unserialize(data.data(), data.size() - 2)) { error = "retro_unserialize failed"; return false; }
		g.buttons = (uint16_t)(data[data.size() - 2] | data[data.size() - 1] << 8);
		g.frame_fresh = false;
		return true;
	}
	bool capture(FrameImage& image, std::string& error) override {
		if (g.frame.rgb.empty()) { error = "no frame has been presented yet"; return false; }
		if (!g.frame_fresh) { error = "no frame has been presented since load_state; advance first"; return false; }
		image = g.frame;
		return true;
	}
	bool supports_reset() const override { return true; }
	bool reset(std::string&) override { g.core.reset(); g.frame_fresh = false; return true; }
	std::vector<std::string> active_exceptions() const override {
		return {"azahar.execution.instruction-step-absent", "azahar.input-hold.port-zero-only", "azahar.state-load.external-media"};
	}
	void extend_hello(Json& hello) const override {
		Json notes = Json::object();
		notes.set("renderer", Json("Azahar libretro core, software renderer"));
		notes.set("screens", Json("screenshot is the core's stacked layout: top screen above the bottom screen"));
		notes.set("touch", Json("set_input pointer {x,y,down} in screenshot coordinates"));
		notes.set("memory", Json("fcram is physical FCRAM (offset-based, batch-readable); vaddr is the current process' virtual space (read-only, unmapped addresses fail); vram comes from the core's libretro memory map"));
		notes.set("clock", Json("EMUCAP_FIXED_TIME pins the RTC and the CPU tick origin"));
		hello.set("capability_notes", notes);
	}

private:
	std::vector<Region> regions_;
	std::vector<std::pair<std::string, uint64_t>> bases_;
};

}  // namespace

int main(int argc, char** argv) {
	std::string core_path, content;
	long frames_unmanaged = -1;
	std::string dump_png;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--core" && i + 1 < argc) core_path = argv[++i];
		else if ((a == "--rom" || a == "--path") && i + 1 < argc) content = argv[++i];
		else if (a == "--frames" && i + 1 < argc) frames_unmanaged = std::atol(argv[++i]);
		else if (a == "--dump-png" && i + 1 < argc) dump_png = argv[++i];
		else if (a == "--verbose") g.log_level = RETRO_LOG_DEBUG;
		else if (a == "--display") {}
		else { std::fprintf(stderr, "usage: emucap-azahar-host --rom <file.3dsx|.3ds|.cci|.cxi|.app> [--core azahar_libretro.so] [--frames N] [--dump-png out.png]\n"); return 2; }
	}
	if (content.empty()) { std::fprintf(stderr, "[emucap] --rom is required\n"); return 2; }
	if (core_path.empty()) {
		const char* env = std::getenv("EMUCAP_AZAHAR_CORE");
		if (env && env[0]) core_path = env;
		else {
			std::string self = argv[0];
			size_t slash = self.find_last_of('/');
			core_path = (slash == std::string::npos ? std::string(".") : self.substr(0, slash)) + "/azahar_libretro.so";
		}
	}
	const char* home = std::getenv("EMUCAP_GENERATION_HOME");
	g.home = home && home[0] ? home : ".";
	g.content = content;
	g.options["citra_graphics_api"] = "Software";
	g.options["citra_use_cpu_jit"] = "enabled";
	g.options["citra_audio_emulation"] = "hle";
	if (!std::getenv("EMUCAP_FIXED_TIME")) setenv("EMUCAP_FIXED_TIME", "946681277", 1);

	if (!load_core(core_path)) return 1;
	load_exports();
	Core& c = g.core;
	c.set_environment(environment);
	c.init();
	c.set_video_refresh(video_refresh);
	c.set_audio_sample(audio_sample);
	c.set_audio_sample_batch(audio_sample_batch);
	c.set_input_poll(input_poll);
	c.set_input_state(input_state);
	c.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	retro_game_info info{};
	info.path = g.content.c_str();
	if (!c.load_game(&info)) { std::fprintf(stderr, "[emucap] retro_load_game failed for %s\n", g.content.c_str()); return 1; }
	retro_system_av_info av{};
	c.get_system_av_info(&av);
	const double fps = av.timing.fps > 1 ? av.timing.fps : 60.0;

	AzaharBackend backend;
	backend.build_regions();
	emucap_producer::Producer producer(backend);
	const bool managed = producer.start();
	producer.entry();

	using clock = std::chrono::steady_clock;
	const auto period = std::chrono::nanoseconds((long long)(1e9 / fps));
	auto next = clock::now();
	long run = 0;
	while (!producer.quit_requested() && !g.shutdown) {
		if (g.has_frame_time && g.frame_time.callback) g.frame_time.callback((retro_usec_t)(1e6 / fps));
		c.run();
		if (g.has_audio_cb && g.audio_cb.callback) g.audio_cb.callback();
		if (!managed) {
			run++;
			if (frames_unmanaged >= 0 && run >= frames_unmanaged) break;
			continue;
		}
		const bool was_frozen = producer.frozen();
		producer.boundary();
		if (!producer.frozen() && !was_frozen) {
			next += period;
			auto now = clock::now();
			if (next > now) std::this_thread::sleep_until(next);
			else if (now - next > std::chrono::milliseconds(250)) next = now;
		} else {
			next = clock::now();
		}
	}
	if (!dump_png.empty()) {
		std::vector<uint8_t> png;
		std::string error;
		if (!g.frame.rgb.empty() && emucap_producer::encode_png_rgb(g.frame.rgb.data(), g.frame.width, g.frame.height, png)
			&& emucap_producer::write_file_atomic(dump_png, png, error))
			std::fprintf(stderr, "[emucap] wrote %s (%dx%d) after %ld frames\n", dump_png.c_str(), g.frame.width, g.frame.height, run);
		else
			std::fprintf(stderr, "[emucap] no frame to write %s\n", error.c_str());
	}
	producer.stop();
	std::fflush(stderr);
	// Skip core teardown: the managed generation is ended by process termination.
	std::_Exit(0);
}
