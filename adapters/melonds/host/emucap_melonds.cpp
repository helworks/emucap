// emucap-melonds-host: headless melonDS (Nintendo DS) producer for emucap.
//
// Boots an .nds image in the melonDS core (FreeBIOS + generated firmware, direct boot) and serves
// emucap's control wire from inside this process through adapters/_common/emucap_producer. The
// emulator thread owns the core; requests run only between RunFrame() calls.
//
// SPDX-License-Identifier: GPL-3.0-or-later (links the GPLv3 melonDS core)

#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "Savestate.h"
#include "SPU.h"
#include "GPU.h"
#include "Args.h"

#include "emucap_producer.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef EMUCAP_BUILD_ID
#define EMUCAP_BUILD_ID "unknown"
#endif

using namespace melonDS;

// ---- Platform ---------------------------------------------------------------------------------

namespace melonDS::Platform {

struct FileHandle { FILE* f; };
struct Thread { std::thread t; };
struct Semaphore { std::mutex m; std::condition_variable cv; int count = 0; };
struct Mutex { std::mutex m; };
struct DynamicLibrary {};
struct AACDecoder {};

void SignalStop(StopReason, void*) {}

std::string GetLocalFilePath(const std::string& filename) { return filename; }

FileHandle* OpenFile(const std::string& path, FileMode mode) {
	std::string m;
	if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite) {
		if (mode & FileMode::Preserve) m = "r+";
		else m = "w+";
	} else if (mode & FileMode::Write) {
		m = (mode & FileMode::Append) ? "a" : ((mode & FileMode::Preserve) ? "r+" : "w");
	} else {
		m = "r";
	}
	if (!(mode & FileMode::Text)) m += "b";
	if ((mode & FileMode::Write) && (mode & FileMode::NoCreate)) {
		FILE* probe = std::fopen(path.c_str(), "rb");
		if (!probe) return nullptr;
		std::fclose(probe);
	}
	FILE* f = std::fopen(path.c_str(), m.c_str());
	if (!f && m[0] == 'r' && m.size() > 1 && m[1] == '+' && !(mode & FileMode::NoCreate))
		f = std::fopen(path.c_str(), (mode & FileMode::Text) ? "w+" : "w+b");
	return f ? new FileHandle{f} : nullptr;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }

bool FileExists(const std::string& name) {
	FILE* f = std::fopen(name.c_str(), "rb");
	if (!f) return false;
	std::fclose(f);
	return true;
}
bool LocalFileExists(const std::string& name) { return FileExists(name); }
bool CheckFileWritable(const std::string& filepath) {
	FILE* f = std::fopen(filepath.c_str(), "ab");
	if (!f) return false;
	std::fclose(f);
	return true;
}
bool CheckLocalFileWritable(const std::string& filepath) { return CheckFileWritable(filepath); }
bool CloseFile(FileHandle* file) {
	if (!file) return false;
	bool ok = std::fclose(file->f) == 0;
	delete file;
	return ok;
}
bool IsEndOfFile(FileHandle* file) { return std::feof(file->f) != 0; }
bool FileReadLine(char* str, int count, FileHandle* file) { return std::fgets(str, count, file->f) != nullptr; }
u64 FilePosition(FileHandle* file) { return (u64)std::ftell(file->f); }
bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin) {
	int o = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
	return fseeko(file->f, offset, o) == 0;
}
void FileRewind(FileHandle* file) { std::rewind(file->f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return std::fread(data, size, count, file->f); }
bool FileFlush(FileHandle* file) { return std::fflush(file->f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) { return std::fwrite(data, size, count, file->f); }
u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int n = std::vfprintf(file->f, fmt, args);
	va_end(args);
	return n < 0 ? 0 : (u64)n;
}
u64 FileLength(FileHandle* file) {
	long pos = std::ftell(file->f);
	fseeko(file->f, 0, SEEK_END);
	long len = std::ftell(file->f);
	fseeko(file->f, pos, SEEK_SET);
	return (u64)len;
}

static int g_log_level = 1;
void Log(LogLevel level, const char* fmt, ...) {
	if ((int)level < g_log_level) return;
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
}

Thread* Thread_Create(std::function<void()> func) { auto* t = new Thread; t->t = std::thread(std::move(func)); return t; }
void Thread_Free(Thread* thread) { if (thread->t.joinable()) thread->t.detach(); delete thread; }
void Thread_Wait(Thread* thread) { if (thread->t.joinable()) thread->t.join(); }

Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* sema) { delete sema; }
void Semaphore_Reset(Semaphore* sema) { std::lock_guard<std::mutex> l(sema->m); sema->count = 0; }
void Semaphore_Wait(Semaphore* sema) {
	std::unique_lock<std::mutex> l(sema->m);
	sema->cv.wait(l, [&] { return sema->count > 0; });
	sema->count--;
}
bool Semaphore_TryWait(Semaphore* sema, int timeout_ms) {
	std::unique_lock<std::mutex> l(sema->m);
	if (!sema->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return sema->count > 0; })) return false;
	sema->count--;
	return true;
}
void Semaphore_Post(Semaphore* sema, int count) {
	{ std::lock_guard<std::mutex> l(sema->m); sema->count += count; }
	sema->cv.notify_all();
}

Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->m.try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetMSCount() {
	return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
u64 GetUSCount() {
	return (u64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Saves stay in memory: a managed generation never writes cartridge or firmware files.
void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*) {
	std::memset(frame, 0, (size_t)width * height * 2);
}
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* data, int maxlength, void*) { std::memset(data, 0, sizeof(s16) * maxlength); return maxlength; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}  // namespace melonDS::Platform

// ---- backend ----------------------------------------------------------------------------------

namespace {

using emucap_producer::CpuState;
using emucap_producer::FrameImage;
using emucap_producer::Region;

// melonDS key bit order (NDS::SetKeyMask, active-low).
const std::vector<std::string> kButtons = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};

class MelonBackend : public emucap_producer::Backend {
public:
	MelonBackend(NDS& nds, std::string rom) : nds_(nds), rom_(std::move(rom)) {
		auto raw = [](u8* base, uint64_t size) {
			return [base, size](uint64_t a, uint8_t* out, size_t n) {
				if (a > size || n > size - a) return false;
				std::memcpy(out, base + a, n);
				return true;
			};
		};
		auto raw_write = [](u8* base, uint64_t size) {
			return [base, size](uint64_t a, const uint8_t* in, size_t n) {
				if (a > size || n > size - a) return false;
				std::memcpy(base + a, in, n);
				return true;
			};
		};
		const uint64_t main_size = (uint64_t)nds_.MainRAMMask + 1;
		regions_.push_back({"main", main_size, true, true, raw(nds_.MainRAM, main_size), raw_write(nds_.MainRAM, main_size)});
		regions_.push_back({"shared_wram", nds_.SharedWRAMSize, true, true, raw(nds_.SharedWRAM, nds_.SharedWRAMSize), raw_write(nds_.SharedWRAM, nds_.SharedWRAMSize)});
		regions_.push_back({"arm7_wram", nds_.ARM7WRAMSize, true, true, raw(nds_.ARM7WRAM, nds_.ARM7WRAMSize), raw_write(nds_.ARM7WRAM, nds_.ARM7WRAMSize)});
		regions_.push_back({"itcm", ITCMPhysicalSize, true, true, raw(nds_.ARM9.ITCM, ITCMPhysicalSize), raw_write(nds_.ARM9.ITCM, ITCMPhysicalSize)});
		regions_.push_back({"dtcm", DTCMPhysicalSize, true, true, raw(nds_.ARM9.DTCM, DTCMPhysicalSize), raw_write(nds_.ARM9.DTCM, DTCMPhysicalSize)});
		// The ARM9 bus view. Reads go through the bus and may touch I/O registers, so it is not
		// offered to read_memory_batch.
		regions_.push_back({"arm9", 1ull << 32, true, false,
			[this](uint64_t a, uint8_t* out, size_t n) {
				for (size_t i = 0; i < n; i++) out[i] = nds_.ARM9Read8((u32)(a + i));
				return true;
			},
			[this](uint64_t a, const uint8_t* in, size_t n) {
				for (size_t i = 0; i < n; i++) nds_.ARM9Write8((u32)(a + i), in[i]);
				return true;
			}});
	}

	std::string system() const override { return "nds"; }
	std::string adapter() const override { return "melonds"; }
	std::string build() const override { return EMUCAP_BUILD_ID; }
	std::string content_path() const override { return rom_; }
	std::vector<Region>& regions() override { return regions_; }
	std::vector<std::string> button_names() const override { return kButtons; }

	void set_buttons(uint64_t mask) override { mask_ = (u32)mask & 0xFFF; }
	bool has_pointer() const override { return true; }
	bool set_pointer(bool down, int x, int y) override {
		touch_down_ = down;
		touch_x_ = x;
		touch_y_ = y >= 192 ? y - 192 : y;
		return true;
	}

	// Apply held input before each frame (emulator thread).
	void apply_input() {
		nds_.SetKeyMask(~mask_ & 0xFFF);
		if (touch_down_) nds_.TouchScreen((u16)touch_x_, (u16)touch_y_);
		else nds_.ReleaseScreen();
	}

	void cpu_state(std::vector<CpuState>& out) override {
		out.clear();
		auto add = [&](const char* id, std::vector<std::string> aliases, ARM& cpu) {
			CpuState s;
			s.cpu_id = id;
			s.aliases = std::move(aliases);
			for (int i = 0; i < 16; i++) s.registers.emplace_back("cpu.r" + std::to_string(i), cpu.R[i]);
			s.registers.emplace_back("cpu.pc", cpu.R[15]);
			s.registers.emplace_back("cpu.cpsr", cpu.CPSR);
			out.push_back(std::move(s));
		};
		add("arm9", {"main", "cpu", "arm946e-s"}, nds_.ARM9);
		add("arm7", {"sub", "arm7tdmi"}, nds_.ARM7);
	}

	bool save_state(std::vector<uint8_t>& out, std::string& error) override {
		Savestate state;
		if (state.Error) { error = "cannot allocate savestate buffer"; return false; }
		if (!nds_.DoSavestate(&state) || state.Error) { error = "melonDS DoSavestate failed"; return false; }
		state.Finish();
		const u8* data = (const u8*)state.Buffer();
		out.assign(data, data + state.Length());
		// Append the host-held input so a loaded origin replays with the same pad state.
		for (int i = 0; i < 4; i++) out.push_back((uint8_t)(mask_ >> (8 * i)));
		return true;
	}

	bool load_state(const std::vector<uint8_t>& data, std::string& error) override {
		if (data.size() < 4) { error = "savestate payload is truncated"; return false; }
		std::vector<uint8_t> payload(data.begin(), data.end() - 4);
		Savestate state(payload.data(), (u32)payload.size(), false);
		if (state.Error || !nds_.DoSavestate(&state) || state.Error) { error = "melonDS rejected the savestate"; return false; }
		size_t t = data.size() - 4;
		mask_ = (u32)data[t] | (u32)data[t + 1] << 8 | (u32)data[t + 2] << 16 | (u32)data[t + 3] << 24;
		fresh_ = false;
		return true;
	}

	void frame_done() { fresh_ = true; }

	bool capture(FrameImage& image, std::string& error) override {
		int front = nds_.GPU.FrontBuffer;
		const u32* top = nds_.GPU.Framebuffer[front][0].get();
		const u32* bottom = nds_.GPU.Framebuffer[front][1].get();
		if (!top || !bottom) { error = "no rendered frame is available"; return false; }
		if (!fresh_) { error = "no frame has been rendered since load_state; advance first"; return false; }
		image.width = 256;
		image.height = 384;
		image.rgb.resize(256 * 384 * 3);
		for (int i = 0; i < 256 * 192; i++) {
			for (int s = 0; s < 2; s++) {
				u32 px = (s ? bottom : top)[i];
				uint8_t* o = &image.rgb[((size_t)s * 256 * 192 + i) * 3];
				o[0] = (uint8_t)(px >> 16);
				o[1] = (uint8_t)(px >> 8);
				o[2] = (uint8_t)px;
			}
		}
		return true;
	}

	bool supports_reset() const override { return true; }
	bool reset(std::string& error) override {
		nds_.Reset();
		if (nds_.NeedsDirectBoot() || direct_boot_) nds_.SetupDirectBoot(rom_name());
		nds_.Start();
		fresh_ = false;
		(void)error;
		return true;
	}

	std::vector<std::string> active_exceptions() const override {
		return {"melonds.execution.instruction-step-absent", "melonds.input-hold.port-zero-only", "melonds.state-load.external-media"};
	}

	void extend_hello(emucap_producer::Json& hello) const override {
		emucap_producer::Json notes = emucap_producer::Json::object();
		notes.set("screens", emucap_producer::Json("screenshot stacks the top screen over the bottom screen (256x384)"));
		notes.set("touch", emucap_producer::Json("set_input pointer {x,y,down} in screenshot coordinates; y>=192 is the touch screen"));
		notes.set("bios", emucap_producer::Json("FreeBIOS and generated firmware; direct boot"));
		hello.set("capability_notes", notes);
	}

	std::string rom_name() const {
		size_t slash = rom_.find_last_of('/');
		return slash == std::string::npos ? rom_ : rom_.substr(slash + 1);
	}

	bool direct_boot_ = true;

private:
	NDS& nds_;
	std::string rom_;
	std::vector<Region> regions_;
	u32 mask_ = 0;
	bool touch_down_ = false;
	int touch_x_ = 0, touch_y_ = 0;
	bool fresh_ = false;
};

void usage() {
	std::fprintf(stderr, "usage: emucap-melonds-host --rom <file.nds> [--jit] [--frames N] [--dump-png out.png]\n");
}

}  // namespace

int main(int argc, char** argv) {
	std::string rom;
	bool jit = false;
	long frames_unmanaged = -1;
	std::string dump_png;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if ((a == "--rom" || a == "--path") && i + 1 < argc) rom = argv[++i];
		else if (a == "--jit") jit = true;
		else if (a == "--frames" && i + 1 < argc) frames_unmanaged = std::atol(argv[++i]);
		else if (a == "--dump-png" && i + 1 < argc) dump_png = argv[++i];
		else if (a == "--verbose") melonDS::Platform::g_log_level = 0;
		else if (rom.empty() && a[0] != '-') rom = a;
		else { usage(); return 2; }
	}
	if (rom.empty()) { usage(); return 2; }

	std::vector<uint8_t> bytes;
	std::string error;
	if (!emucap_producer::read_file(rom, bytes, error)) { std::fprintf(stderr, "[emucap] %s\n", error.c_str()); return 1; }
	auto data = std::make_unique<u8[]>(bytes.size());
	std::memcpy(data.get(), bytes.data(), bytes.size());

	NDSArgs args;
	if (!jit) args.JIT = std::nullopt;
	auto nds = std::make_unique<NDS>(std::move(args), nullptr);
	NDSCart::NDSCartArgs cart_args {};
	auto cart = NDSCart::ParseROM(std::move(data), (u32)bytes.size(), nullptr, std::move(cart_args));
	if (!cart) { std::fprintf(stderr, "[emucap] melonDS could not parse %s\n", rom.c_str()); return 1; }
	nds->Reset();
	nds->SetNDSCart(std::move(cart));
	MelonBackend backend(*nds, rom);
	nds->SetupDirectBoot(backend.rom_name());
	nds->Start();

	emucap_producer::Producer producer(backend);
	const bool managed = producer.start();
	producer.entry();

	using clock = std::chrono::steady_clock;
	const auto period = std::chrono::nanoseconds((long long)(1e9 / 59.8261));
	auto next = clock::now();
	std::vector<s16> audio(4096);
	long run = 0;
	while (!producer.quit_requested()) {
		backend.apply_input();
		nds->RunFrame();
		while (nds->SPU.ReadOutput(audio.data(), 2048) > 0) {}
		backend.frame_done();
		if (!managed) {
			run++;
			if (frames_unmanaged >= 0 && run >= frames_unmanaged) break;
			continue;
		}
		const bool was_frozen = producer.frozen();
		producer.boundary();
		// Free-running frames keep real time; stepped frames run flat out.
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
		FrameImage image;
		std::vector<uint8_t> png;
		if (backend.capture(image, error) && emucap_producer::encode_png_rgb(image.rgb.data(), image.width, image.height, png)
			&& emucap_producer::write_file_atomic(dump_png, png, error))
			std::fprintf(stderr, "[emucap] wrote %s after %ld frames\n", dump_png.c_str(), run);
		else
			std::fprintf(stderr, "[emucap] screenshot failed: %s\n", error.c_str());
	}
	producer.stop();
	return 0;
}
