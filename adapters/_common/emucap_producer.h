// emucap in-process producer kit for C++ emulators.
//
// An emulator that links this kit speaks emucap's control wire from inside its own process, the
// same way the PSoXide host does: a transport thread connects to the Control MCP listener named by
// EMUCAP_PORT, completes launch-bound producer authentication (emucap_front_auth.h), and hands every
// request to the emulator thread. The emulator thread runs requests only at frame boundaries, where
// guest state is consistent, so a read never observes a half-executed frame and an advance is
// never raced by free-running emulation.
//
// Integration contract for an emulator frontend:
//   1. Implement emucap_producer::Backend for the emulator.
//   2. Create a Producer, call start() once the machine is booted and before the first frame.
//   3. Call entry() once before the first emulated frame (the pre_first_instruction boundary;
//      parks there when EMUCAP_START_FROZEN=1), then boundary() after every emulated frame.
//      Both block while the guest is frozen and return when the emulator should run a frame.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace emucap_producer {

// ---- minimal JSON value -----------------------------------------------------------------------

class Json {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Json() = default;
	Json(std::nullptr_t) {}
	Json(bool value) : type_(Type::Bool), bool_(value) {}
	Json(int value) : type_(Type::Number), num_(value), int_(value), is_int_(true) {}
	Json(long value) : type_(Type::Number), num_((double)value), int_(value), is_int_(true) {}
	Json(long long value) : type_(Type::Number), num_((double)value), int_(value), is_int_(true) {}
	Json(unsigned value) : Json((unsigned long long)value) {}
	Json(unsigned long value) : Json((unsigned long long)value) {}
	Json(unsigned long long value)
		: type_(Type::Number), num_((double)value), uint_(value), is_uint_(true) {}
	Json(double value) : type_(Type::Number), num_(value) {}
	Json(const char* value) : type_(Type::String), str_(value ? value : "") {}
	Json(std::string value) : type_(Type::String), str_(std::move(value)) {}

	static Json array() { Json j; j.type_ = Type::Array; return j; }
	static Json object() { Json j; j.type_ = Type::Object; return j; }
	static Json strings(const std::vector<std::string>& values);

	Type type() const { return type_; }
	bool is_null() const { return type_ == Type::Null; }
	bool is_bool() const { return type_ == Type::Bool; }
	bool is_number() const { return type_ == Type::Number; }
	bool is_string() const { return type_ == Type::String; }
	bool is_array() const { return type_ == Type::Array; }
	bool is_object() const { return type_ == Type::Object; }

	bool as_bool() const { return bool_; }
	double as_double() const { return num_; }
	// Exact non-negative integer view of a number or hex/decimal string; false otherwise.
	bool as_u64(uint64_t& out) const;
	const std::string& as_string() const { return str_; }
	const std::vector<Json>& items() const { return arr_; }
	const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

	// Object access. get() returns a shared null for missing keys.
	const Json& get(const std::string& key) const;
	bool has(const std::string& key) const;
	Json& set(const std::string& key, Json value);
	Json& push(Json value);

	std::string dump() const;
	static bool parse(const std::string& text, Json& out, std::string& error);

private:
	void dump_to(std::string& out) const;

	Type type_ = Type::Null;
	bool bool_ = false;
	double num_ = 0;
	long long int_ = 0;
	unsigned long long uint_ = 0;
	bool is_int_ = false;
	bool is_uint_ = false;
	std::string str_;
	std::string raw_number_;
	std::vector<Json> arr_;
	std::vector<std::pair<std::string, Json>> obj_;

	friend class JsonParser;
};

// ---- helpers ----------------------------------------------------------------------------------

std::string hex_encode(const uint8_t* data, size_t size);
bool hex_decode(const std::string& text, std::vector<uint8_t>& out);
std::string base64_encode(const uint8_t* data, size_t size);
std::string sha1_hex(const uint8_t* data, size_t size);
std::string sha256_hex(const uint8_t* data, size_t size);
// Encode tightly packed RGB8 pixels as PNG (zlib deflate).
bool encode_png_rgb(const uint8_t* rgb, int width, int height, std::vector<uint8_t>& out);
bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& error);
bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, std::string& error);

// ---- backend ----------------------------------------------------------------------------------

struct Region {
	std::string name;
	uint64_t size = 0;
	bool writable = true;
	// Side-effect-free reads usable at a frozen boundary by read_memory_batch.
	bool batch = true;
	std::function<bool(uint64_t address, uint8_t* out, size_t length)> read;
	std::function<bool(uint64_t address, const uint8_t* data, size_t length)> write;
};

struct FrameImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb;  // width * height * 3, row-major, top-down
};

struct CpuState {
	std::string cpu_id = "main";
	std::vector<std::string> aliases;
	std::vector<std::pair<std::string, uint64_t>> registers;  // "cpu.pc" -> value
};

class Backend {
public:
	virtual ~Backend() = default;

	virtual std::string system() const = 0;   // emucap system id ("nds", "3ds", ...)
	virtual std::string adapter() const = 0;  // adapter id reported in hello/status
	virtual std::string build() const = 0;    // emucap revision @ upstream revision
	virtual std::string content_path() const = 0;

	virtual std::vector<Region>& regions() = 0;
	virtual std::vector<std::string> button_names() const = 0;  // bit i = button_names()[i]
	// Apply a held-button mask from the next emulated frame on (emulator thread).
	virtual void set_buttons(uint64_t mask) = 0;
	// Pointer input (touch screens). Return false when unsupported.
	virtual bool set_pointer(bool /*down*/, int /*x*/, int /*y*/) { return false; }
	virtual bool has_pointer() const { return false; }

	virtual void cpu_state(std::vector<CpuState>& out) = 0;

	virtual bool supports_state() const { return true; }
	virtual bool save_state(std::vector<uint8_t>& out, std::string& error) = 0;
	virtual bool load_state(const std::vector<uint8_t>& data, std::string& error) = 0;

	// Last completed guest frame. Return false with an error when none exists.
	virtual bool capture(FrameImage& image, std::string& error) = 0;

	virtual bool supports_reset() const { return false; }
	virtual bool reset(std::string& error) { error = "reset is not supported"; return false; }

	// Contract exceptions this adapter activates (contracts/exceptions.json ids).
	virtual std::vector<std::string> active_exceptions() const { return {}; }
	// Extra fields merged into hello (capability notes, etc.).
	virtual void extend_hello(Json& /*hello*/) const {}
	// Whether entry() is a true pre-first-instruction boundary (start_frozen support).
	virtual bool controlled_start() const { return true; }
};

// ---- producer ---------------------------------------------------------------------------------

class Producer {
public:
	explicit Producer(Backend& backend);
	~Producer();
	Producer(const Producer&) = delete;
	Producer& operator=(const Producer&) = delete;

	// Read EMUCAP_* and start the transport thread when a managed launch configured one.
	// Returns false when no Control endpoint is configured (the emulator then runs freely).
	bool start();
	void stop();

	// Emulator thread: once before the first emulated frame.
	void entry();
	// Emulator thread: after every emulated frame.
	void boundary();

	bool frozen() const { return frozen_.load(); }
	uint64_t frame() const { return frame_.load(); }
	bool quit_requested() const { return quit_.load(); }
	bool connected() const { return connected_.load(); }

	// Run a request synchronously on the calling (emulator) thread. Used by tests and --stdio.
	std::string handle_line_now(const std::string& line);

private:
	struct Job {
		uint64_t id = 0;
		std::string method;
		Json params;
		std::mutex mutex;
		std::condition_variable done_cv;
		bool done = false;
		std::string reply;  // complete NDJSON line without newline
	};

	void transport_main();
	bool serve_connection(int fd);
	std::string local_reply(uint64_t id, const std::string& method, const Json& params, bool& handled);
	void park();
	void execute(const std::shared_ptr<Job>& job);
	void finish(const std::shared_ptr<Job>& job, const std::string& reply);
	std::string ok(uint64_t id, const Json& result) const;
	std::string err(uint64_t id, const char* kind, const std::string& message) const;

	Json hello() const;
	Json status() const;
	Json rom_info();
	Json execution_limits() const;
	Json batch_capability();

	// Emulator-thread method bodies; return a full reply line or empty when deferred.
	std::string run_job(const std::shared_ptr<Job>& job);
	void complete_step(const char* status, const char* reason);

	Backend& backend_;
	std::thread transport_;
	std::atomic<bool> quit_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> frozen_{false};
	std::atomic<uint64_t> frame_{0};
	std::atomic<uint64_t> stop_epoch_{0};
	bool started_ = false;
	bool start_frozen_ = false;
	std::atomic<bool> at_entry_{false};
	std::vector<CpuState> cpu_targets_;

	std::string port_text_;
	uint16_t port_ = 0;
	std::string front_key_;
	std::string launch_id_;
	std::string name_;
	std::string content_;

	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<std::shared_ptr<Job>> queue_;

	// Deferred step (emulator thread only).
	std::shared_ptr<Job> step_job_;
	uint64_t step_requested_ = 0;
	uint64_t step_remaining_ = 0;
	uint64_t step_frame_before_ = 0;
	bool step_was_running_ = false;
	int64_t step_deadline_ms_ = 0;

	std::atomic<uint64_t> held_mask_{0};
	std::atomic<bool> override_engaged_{false};

	std::mutex rom_mutex_;
	bool rom_cached_ = false;
	Json rom_info_;
};

}  // namespace emucap_producer
