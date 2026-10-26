// emucap in-process producer kit. See emucap_producer.h.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "emucap_producer.h"

#include "emucap_front_auth.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static int current_pid() { return _getpid(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
static int current_pid() { return (int)getpid(); }
#endif

namespace emucap_producer {

namespace {

constexpr uint64_t kMaxSyncAdvance = 5000;
constexpr uint64_t kMaxTransfer = 0x10000;
constexpr size_t kMaxLine = 8 * 1024 * 1024;
constexpr int64_t kAdvanceBudgetMs = 245000;
constexpr const char* kStateMagic = "EMUCAPP1";

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string env(const char* name) {
	const char* value = std::getenv(name);
	return value ? std::string(value) : std::string();
}

void log(const char* fmt, const std::string& a = std::string()) {
	std::fprintf(stderr, "[emucap] ");
	std::fprintf(stderr, fmt, a.c_str());
	std::fprintf(stderr, "\n");
	std::fflush(stderr);
}

}  // namespace

// ---- Json -------------------------------------------------------------------------------------

Json Json::strings(const std::vector<std::string>& values) {
	Json out = array();
	for (const auto& value : values) out.push(Json(value));
	return out;
}

bool Json::as_u64(uint64_t& out) const {
	if (type_ == Type::Number) {
		if (is_uint_) { out = uint_; return true; }
		if (is_int_) { if (int_ < 0) return false; out = (uint64_t)int_; return true; }
		if (num_ < 0 || num_ != std::floor(num_) || num_ > 1.8e19) return false;
		out = (uint64_t)num_;
		return true;
	}
	if (type_ == Type::String && !str_.empty()) {
		char* end = nullptr;
		errno = 0;
		unsigned long long value = std::strtoull(str_.c_str(), &end, 0);
		if (errno != 0 || !end || *end != 0 || str_[0] == '-') return false;
		out = value;
		return true;
	}
	return false;
}

const Json& Json::get(const std::string& key) const {
	static const Json null_value;
	if (type_ != Type::Object) return null_value;
	for (const auto& member : obj_)
		if (member.first == key) return member.second;
	return null_value;
}

bool Json::has(const std::string& key) const {
	if (type_ != Type::Object) return false;
	for (const auto& member : obj_)
		if (member.first == key) return true;
	return false;
}

Json& Json::set(const std::string& key, Json value) {
	if (type_ != Type::Object) { *this = object(); }
	for (auto& member : obj_) {
		if (member.first == key) { member.second = std::move(value); return member.second; }
	}
	obj_.emplace_back(key, std::move(value));
	return obj_.back().second;
}

Json& Json::push(Json value) {
	if (type_ != Type::Array) { *this = array(); }
	arr_.push_back(std::move(value));
	return arr_.back();
}

static void escape_into(std::string& out, const std::string& text) {
	out.push_back('"');
	for (unsigned char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			} else {
				out.push_back((char)c);
			}
		}
	}
	out.push_back('"');
}

void Json::dump_to(std::string& out) const {
	switch (type_) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += bool_ ? "true" : "false"; break;
	case Type::Number: {
		char buffer[40];
		if (is_uint_) std::snprintf(buffer, sizeof(buffer), "%llu", uint_);
		else if (is_int_) std::snprintf(buffer, sizeof(buffer), "%lld", int_);
		else if (!raw_number_.empty()) { out += raw_number_; break; }
		else if (std::isfinite(num_)) std::snprintf(buffer, sizeof(buffer), "%.17g", num_);
		else std::snprintf(buffer, sizeof(buffer), "null");
		out += buffer;
		break;
	}
	case Type::String: escape_into(out, str_); break;
	case Type::Array:
		out.push_back('[');
		for (size_t i = 0; i < arr_.size(); i++) {
			if (i) out.push_back(',');
			arr_[i].dump_to(out);
		}
		out.push_back(']');
		break;
	case Type::Object:
		out.push_back('{');
		for (size_t i = 0; i < obj_.size(); i++) {
			if (i) out.push_back(',');
			escape_into(out, obj_[i].first);
			out.push_back(':');
			obj_[i].second.dump_to(out);
		}
		out.push_back('}');
		break;
	}
}

std::string Json::dump() const {
	std::string out;
	dump_to(out);
	return out;
}

class JsonParser {
public:
	explicit JsonParser(const std::string& text) : s_(text) {}

	bool parse(Json& out, std::string& error) {
		if (!value(out, 0)) { error = error_.empty() ? "invalid JSON" : error_; return false; }
		ws();
		if (i_ != s_.size()) { error = "trailing characters after JSON value"; return false; }
		return true;
	}

private:
	void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) i_++; }

	bool value(Json& out, int depth) {
		if (depth > 64) { error_ = "JSON nesting too deep"; return false; }
		ws();
		if (i_ >= s_.size()) return false;
		char c = s_[i_];
		if (c == '{') {
			i_++;
			out = Json::object();
			ws();
			if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
			while (true) {
				ws();
				std::string key;
				if (!string(key)) return false;
				ws();
				if (i_ >= s_.size() || s_[i_] != ':') return false;
				i_++;
				Json member;
				if (!value(member, depth + 1)) return false;
				out.obj_.emplace_back(std::move(key), std::move(member));
				ws();
				if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
				if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
				return false;
			}
		}
		if (c == '[') {
			i_++;
			out = Json::array();
			ws();
			if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
			while (true) {
				Json item;
				if (!value(item, depth + 1)) return false;
				out.arr_.push_back(std::move(item));
				ws();
				if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
				if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
				return false;
			}
		}
		if (c == '"') {
			std::string text;
			if (!string(text)) return false;
			out = Json(std::move(text));
			return true;
		}
		if (s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Json(true); return true; }
		if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Json(false); return true; }
		if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Json(); return true; }
		return number(out);
	}

	bool number(Json& out) {
		size_t start = i_;
		if (i_ < s_.size() && s_[i_] == '-') i_++;
		bool integral = true;
		while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) i_++;
		if (i_ < s_.size() && s_[i_] == '.') { integral = false; i_++; while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) i_++; }
		if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
			integral = false; i_++;
			if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) i_++;
			while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) i_++;
		}
		if (i_ == start || (i_ == start + 1 && s_[start] == '-')) return false;
		std::string raw = s_.substr(start, i_ - start);
		if (integral) {
			errno = 0;
			if (raw[0] == '-') {
				long long value = std::strtoll(raw.c_str(), nullptr, 10);
				if (errno == 0) { out = Json(value); return true; }
			} else {
				unsigned long long value = std::strtoull(raw.c_str(), nullptr, 10);
				if (errno == 0) { out = Json(value); return true; }
			}
		}
		out = Json(std::strtod(raw.c_str(), nullptr));
		out.raw_number_ = raw;
		return true;
	}

	static void utf8(std::string& out, uint32_t cp) {
		if (cp < 0x80) out.push_back((char)cp);
		else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
		else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
		else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
	}

	bool hex4(uint32_t& out) {
		if (i_ + 4 > s_.size()) return false;
		out = 0;
		for (int k = 0; k < 4; k++) {
			char h = s_[i_++];
			out <<= 4;
			if (h >= '0' && h <= '9') out |= (uint32_t)(h - '0');
			else if (h >= 'a' && h <= 'f') out |= (uint32_t)(h - 'a' + 10);
			else if (h >= 'A' && h <= 'F') out |= (uint32_t)(h - 'A' + 10);
			else return false;
		}
		return true;
	}

	bool string(std::string& out) {
		if (i_ >= s_.size() || s_[i_] != '"') return false;
		i_++;
		while (i_ < s_.size()) {
			char c = s_[i_++];
			if (c == '"') return true;
			if (c != '\\') { out.push_back(c); continue; }
			if (i_ >= s_.size()) return false;
			char e = s_[i_++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				uint32_t cp;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
					i_ += 2;
					uint32_t low;
					if (!hex4(low)) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				}
				utf8(out, cp);
				break;
			}
			default: return false;
			}
		}
		return false;
	}

	const std::string& s_;
	size_t i_ = 0;
	std::string error_;
};

bool Json::parse(const std::string& text, Json& out, std::string& error) {
	JsonParser parser(text);
	return parser.parse(out, error);
}

// ---- helpers ----------------------------------------------------------------------------------

std::string hex_encode(const uint8_t* data, size_t size) {
	static const char* digits = "0123456789abcdef";
	std::string out;
	out.resize(size * 2);
	for (size_t i = 0; i < size; i++) {
		out[2 * i] = digits[data[i] >> 4];
		out[2 * i + 1] = digits[data[i] & 15];
	}
	return out;
}

bool hex_decode(const std::string& text, std::vector<uint8_t>& out) {
	std::string clean;
	clean.reserve(text.size());
	for (char c : text) if (c != ' ' && c != '\n' && c != '\t') clean.push_back(c);
	if (clean.rfind("0x", 0) == 0 || clean.rfind("0X", 0) == 0) clean = clean.substr(2);
	if (clean.empty() || clean.size() % 2) return false;
	out.clear();
	out.reserve(clean.size() / 2);
	for (size_t i = 0; i < clean.size(); i += 2) {
		int value = 0;
		for (int k = 0; k < 2; k++) {
			char h = clean[i + k];
			value <<= 4;
			if (h >= '0' && h <= '9') value |= h - '0';
			else if (h >= 'a' && h <= 'f') value |= h - 'a' + 10;
			else if (h >= 'A' && h <= 'F') value |= h - 'A' + 10;
			else return false;
		}
		out.push_back((uint8_t)value);
	}
	return true;
}

std::string base64_encode(const uint8_t* data, size_t len) {
	static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = (uint32_t)data[i] << 16;
		if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len) n |= (uint32_t)data[i + 2];
		out.push_back(table[(n >> 18) & 63]);
		out.push_back(table[(n >> 12) & 63]);
		out.push_back(i + 1 < len ? table[(n >> 6) & 63] : '=');
		out.push_back(i + 2 < len ? table[n & 63] : '=');
	}
	return out;
}

std::string sha1_hex(const uint8_t* data, size_t size) {
	uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	auto rol = [](uint32_t v, int c) { return (v << c) | (v >> (32 - c)); };
	std::vector<uint8_t> msg(data, data + size);
	uint64_t bits = (uint64_t)size * 8;
	msg.push_back(0x80);
	while (msg.size() % 64 != 56) msg.push_back(0);
	for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bits >> (i * 8)));
	for (size_t off = 0; off < msg.size(); off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)msg[off + 4 * i] << 24 | (uint32_t)msg[off + 4 * i + 1] << 16 |
			       (uint32_t)msg[off + 4 * i + 2] << 8 | (uint32_t)msg[off + 4 * i + 3];
		for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
			uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	uint8_t out[20];
	for (int i = 0; i < 5; i++)
		for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
	return hex_encode(out, 20);
}

std::string sha256_hex(const uint8_t* data, size_t size) {
	emucap_front_auth::detail::Sha256 hash;
	hash.Update(data, size);
	auto digest = hash.Finish();
	return hex_encode(digest.data(), digest.size());
}

static void png_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size) {
	auto be32 = [&](uint32_t v) {
		out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
		out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v);
	};
	be32((uint32_t)size);
	size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	if (size) out.insert(out.end(), data, data + size);
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, out.data() + start, (uInt)(out.size() - start));
	be32((uint32_t)crc);
}

bool encode_png_rgb(const uint8_t* rgb, int width, int height, std::vector<uint8_t>& out) {
	if (width <= 0 || height <= 0) return false;
	std::vector<uint8_t> raw;
	const size_t stride = (size_t)width * 3;
	raw.reserve((stride + 1) * (size_t)height);
	for (int y = 0; y < height; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), rgb + stride * (size_t)y, rgb + stride * (size_t)(y + 1));
	}
	uLongf zsize = compressBound((uLong)raw.size());
	std::vector<uint8_t> z(zsize);
	if (compress2(z.data(), &zsize, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
	z.resize(zsize);
	out.clear();
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	out.insert(out.end(), sig, sig + 8);
	uint8_t ihdr[13] = {
		(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
		(uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
		8, 2, 0, 0, 0};
	png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
	png_chunk(out, "IDAT", z.data(), z.size());
	png_chunk(out, "IEND", nullptr, 0);
	return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) { error = "cannot open " + path; return false; }
	out.clear();
	uint8_t buffer[65536];
	size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
		out.insert(out.end(), buffer, buffer + n);
		if (out.size() > (size_t)1 << 31) { std::fclose(f); error = "file exceeds 2 GiB"; return false; }
	}
	bool bad = std::ferror(f) != 0;
	std::fclose(f);
	if (bad) { error = "read failed: " + path; return false; }
	return true;
}

bool write_file_atomic(const std::string& path, const std::vector<uint8_t>& data, std::string& error) {
	std::string tmp = path + ".tmp-" + std::to_string(current_pid());
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) { error = "cannot create " + tmp; return false; }
	bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = std::fflush(f) == 0 && ok;
	ok = std::fclose(f) == 0 && ok;
	if (!ok) { std::remove(tmp.c_str()); error = "write failed: " + tmp; return false; }
#ifdef _WIN32
	std::remove(path.c_str());
#endif
	if (std::rename(tmp.c_str(), path.c_str()) != 0) {
		std::remove(tmp.c_str());
		error = "rename failed: " + path;
		return false;
	}
	return true;
}

// ---- Producer ---------------------------------------------------------------------------------

Producer::Producer(Backend& backend) : backend_(backend) {}

Producer::~Producer() { stop(); }

bool Producer::start() {
	if (started_) return true;
	start_frozen_ = env("EMUCAP_START_FROZEN") == "1" && backend_.controlled_start();
	frozen_ = start_frozen_;
	port_text_ = env("EMUCAP_PORT");
	front_key_ = env("EMUCAP_SESSION_TOKEN");
	launch_id_ = env("EMUCAP_LAUNCH_ID");
	name_ = env("EMUCAP_NAME");
	content_ = env("EMUCAP_CONTENT");
	int port = std::atoi(port_text_.c_str());
	if (port <= 0 || port > 65535 || front_key_.empty() || launch_id_.empty()) {
		log("%s", "no managed Control endpoint (EMUCAP_PORT/EMUCAP_SESSION_TOKEN/EMUCAP_LAUNCH_ID); running unmanaged");
		frozen_ = false;
		start_frozen_ = false;
		return false;
	}
	port_ = (uint16_t)port;
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	backend_.cpu_state(cpu_targets_);
	started_ = true;
	transport_ = std::thread([this] { transport_main(); });
	if (backend_.self_paced()) {
		if (start_frozen_) {
			backend_.halt();
			halted_ = true;
		}
		service_ = std::thread([this] { service_main(); });
	}
	return true;
}

// Self-paced hosts: execute each request as it arrives; the emulator runs its own frame loop.
void Producer::service_main() {
	while (!quit_) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			if (queue_.empty()) {
				queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
				continue;
			}
			job = queue_.front();
			queue_.pop_front();
		}
		frame_ = backend_.frame_count();
		execute(job);
		frame_ = backend_.frame_count();
	}
}

void Producer::stop() {
	quit_ = true;
	queue_cv_.notify_all();
	if (transport_.joinable()) transport_.join();
	if (service_.joinable()) service_.join();
}

std::string Producer::ok(uint64_t id, const Json& result) const {
	Json reply = Json::object();
	reply.set("id", Json((unsigned long long)id));
	reply.set("ok", Json(true));
	reply.set("result", result);
	return reply.dump();
}

std::string Producer::err(uint64_t id, const char* kind, const std::string& message) const {
	Json error = Json::object();
	error.set("kind", Json(kind));
	error.set("message", Json(message));
	Json reply = Json::object();
	reply.set("id", Json((unsigned long long)id));
	reply.set("ok", Json(false));
	reply.set("error", error);
	return reply.dump();
}

static const char* kMethods[] = {
	"hello", "status", "get_rom_info", "get_state", "read_memory", "write_memory",
	"read_memory_batch", "pause", "resume", "step", "set_input", "screenshot",
	"save_state", "load_state", "poll_events", "list_breakpoints",
	"clear_all_breakpoints"};

Json Producer::execution_limits() const {
	Json limits = Json::object();
	limits.set("max_sync_advance_count", Json((unsigned long long)kMaxSyncAdvance));
	return limits;
}

bool Producer::has_batch_window() {
	for (const auto& region : backend_.regions())
		if (region.batch) return true;
	return false;
}

Json Producer::batch_capability() {
	Json cap = Json::object();
	cap.set("max_ranges", Json(64));
	cap.set("max_range_bytes", Json(65536));
	cap.set("max_total_bytes", Json(65536));
	cap.set("consistency", Json("frozen_boundary"));
	Json kinds = Json::array();
	kinds.push(Json("frame_boundary"));
	if (backend_.controlled_start()) kinds.push(Json("pre_first_instruction"));
	cap.set("halt_kinds", kinds);
	Json windows = Json::array();
	for (const auto& region : backend_.regions()) {
		if (!region.batch) continue;
		Json w = Json::object();
		w.set("memory_type", Json(region.name));
		w.set("address", Json(0));
		w.set("length", Json((unsigned long long)region.size));
		windows.push(w);
	}
	cap.set("windows", windows);
	return cap;
}

Json Producer::hello() const {
	Producer* self = const_cast<Producer*>(this);
	Json h = Json::object();
	h.set("protocol_version", Json(1));
	h.set("system", Json(backend_.system()));
	h.set("adapter", Json(backend_.adapter()));
	h.set("build", Json(backend_.build()));
	h.set("name", Json(name_.empty() ? backend_.adapter() : name_));
	Json methods = Json::array();
	for (const char* m : kMethods) {
		std::string method = m;
		if ((method == "save_state" || method == "load_state") && !backend_.supports_state()) continue;
		if (method == "read_memory_batch" && !self->has_batch_window()) continue;
		methods.push(Json(method));
	}
	if (backend_.supports_reset()) methods.push(Json("reset"));
	h.set("methods", methods);
	Json types = Json::array();
	Json regions = Json::array();
	for (const auto& region : self->backend_.regions()) {
		types.push(Json(region.name));
		Json r = Json::object();
		r.set("memory_type", Json(region.name));
		r.set("size", Json((unsigned long long)region.size));
		regions.push(r);
	}
	h.set("memory_types", types);
	h.set("memory_regions", regions);
	const std::vector<CpuState>& cpus = cpu_targets_;
	Json targets = Json::array();
	Json groups = Json::array();
	groups.push(Json("cpu"));
	for (size_t i = 0; i < cpus.size(); i++) {
		Json t = Json::object();
		t.set("id", Json(cpus[i].cpu_id));
		t.set("aliases", Json::strings(cpus[i].aliases));
		t.set("default", Json(i == 0));
		targets.push(t);
	}
	h.set("state_groups", groups);
	h.set("cpu_targets", targets);
	h.set("breakpoint_kinds", Json::array());
	h.set("input_buttons", Json::strings(backend_.button_names()));
	Json features = Json::array();
	if (backend_.controlled_start()) features.push(Json("controlled_start"));
	h.set("host_features", features);
	h.set("execution_limits", execution_limits());
	Json contracts = Json::object();
	contracts.set("catalog", Json("emucap-feature-contracts/v3"));
	contracts.set("active_exceptions", Json::strings(backend_.active_exceptions()));
	h.set("contracts", contracts);
	// A batch capability must name at least one side-effect-free window.
	if (self->has_batch_window()) h.set("memory_batch_capability", self->batch_capability());
	if (!content_.empty()) h.set("content", Json(content_));
	else h.set("content", Json(backend_.content_path()));
	if (!launch_id_.empty()) h.set("launch_id", Json(launch_id_));
	backend_.extend_hello(h);
	return h;
}

Json Producer::status() const {
	Json s = Json::object();
	s.set("connected", Json(true));
	s.set("system", Json(backend_.system()));
	s.set("adapter", Json(backend_.adapter()));
	s.set("state", Json(frozen_.load() ? "frozen" : "running"));
	s.set("frame", Json((unsigned long long)frame_.load()));
	s.set("boundary", Json(at_entry_.load() ? "pre_first_instruction" : "frame_boundary"));
	s.set("execution_limits", execution_limits());
	Json input = Json::object();
	input.set("observable", Json(true));
	input.set("engaged", Json(override_engaged_.load()));
	input.set("mode", Json(override_engaged_.load() ? "persistent" : "native"));
	input.set("pressed_mask", Json((unsigned long long)held_mask_));
	s.set("input_override", input);
	Json start = Json::object();
	start.set("requested_frozen", Json(start_frozen_));
	start.set("controlled", Json(start_frozen_));
	start.set("boundary", start_frozen_ ? Json("pre_first_instruction") : Json());
	s.set("launch_start", start);
	return s;
}

Json Producer::rom_info() {
	std::lock_guard<std::mutex> lock(rom_mutex_);
	if (rom_cached_) return rom_info_;
	std::string path = backend_.content_path();
	Json info = Json::object();
	info.set("system", Json(backend_.system()));
	info.set("adapter", Json(backend_.adapter()));
	std::string name = path;
	size_t slash = name.find_last_of("/\\");
	if (slash != std::string::npos) name = name.substr(slash + 1);
	info.set("name", Json(name));
	info.set("path", Json(path));
	std::vector<uint8_t> bytes;
	std::string error;
	if (!path.empty() && read_file(path, bytes, error)) {
		std::string sha1 = sha1_hex(bytes.data(), bytes.size());
		info.set("size", Json((unsigned long long)bytes.size()));
		info.set("sha1", Json(sha1));
		info.set("rom_sha1", Json(sha1));
		info.set("sha256", Json(sha256_hex(bytes.data(), bytes.size())));
		info.set("identity_scope", Json("content_file_bytes"));
	} else if (!error.empty()) {
		info.set("identity_error", Json(error));
	}
	rom_info_ = info;
	rom_cached_ = true;
	return rom_info_;
}

std::string Producer::local_reply(uint64_t id, const std::string& method, const Json&, bool& handled) {
	handled = true;
	if (backend_.self_paced()) frame_ = backend_.frame_count();
	if (method == "hello") return ok(id, hello());
	if (method == "status") return ok(id, status());
	if (method == "get_rom_info") return ok(id, rom_info());
	handled = false;
	return std::string();
}

// ---- transport thread -------------------------------------------------------------------------

static bool set_timeout(socket_t fd, int option, int ms) {
#ifdef _WIN32
	DWORD value = (DWORD)ms;
	return setsockopt(fd, SOL_SOCKET, option, (const char*)&value, sizeof(value)) == 0;
#else
	timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv)) == 0;
#endif
}

static bool send_all(socket_t fd, const char* data, size_t size) {
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL;
#else
	const int flags = 0;
#endif
	size_t sent = 0;
	while (sent < size) {
		auto n = ::send(fd, data + sent, (int)(size - sent), flags);
		if (n <= 0) {
#ifndef _WIN32
			if (n < 0 && errno == EINTR) continue;
#endif
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

void Producer::transport_main() {
	int delay_ms = 50;
	while (!quit_) {
		socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd == kInvalidSocket) { std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms)); continue; }
		sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port_);
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
			close_socket(fd);
			std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
			delay_ms = std::min(delay_ms * 2, 2000);
			continue;
		}
		set_timeout(fd, SO_RCVTIMEO, 5000);
		set_timeout(fd, SO_SNDTIMEO, 5000);
		auto read_exact = [&](std::uint8_t* bytes, size_t size) {
			size_t got = 0;
			while (got < size) {
				auto n = ::recv(fd, (char*)bytes + got, (int)(size - got), 0);
				if (n <= 0) return false;
				got += (size_t)n;
			}
			return true;
		};
		auto write_all = [&](const std::uint8_t* bytes, size_t size) {
			return send_all(fd, (const char*)bytes, size);
		};
		if (!emucap_front_auth::Authenticate(front_key_, launch_id_, port_, read_exact, write_all)) {
			log("%s", "producer authentication failed; retrying");
			close_socket(fd);
			std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
			delay_ms = std::min(delay_ms * 2, 2000);
			continue;
		}
		delay_ms = 50;
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
		log("connected to Control MCP on port %s", port_text_);
		connected_ = true;
		serve_connection((int)fd);
		connected_ = false;
		close_socket(fd);
		if (!quit_) log("%s", "Control MCP disconnected; reconnecting");
		std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
	}
}

bool Producer::serve_connection(int raw_fd) {
	socket_t fd = (socket_t)raw_fd;
	set_timeout(fd, SO_RCVTIMEO, 250);
	std::string pending;
	char buffer[65536];
	while (!quit_) {
		size_t newline = pending.find('\n');
		if (newline == std::string::npos) {
			auto n = ::recv(fd, buffer, sizeof(buffer), 0);
			if (n == 0) return true;
			if (n < 0) {
#ifdef _WIN32
				int e = WSAGetLastError();
				if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) continue;
#else
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
				return false;
			}
			pending.append(buffer, (size_t)n);
			if (pending.size() > kMaxLine + 1 && pending.find('\n') == std::string::npos) return false;
			continue;
		}
		std::string line = pending.substr(0, newline);
		pending.erase(0, newline + 1);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;

		Json request;
		std::string parse_error;
		uint64_t id = 0;
		std::string reply;
		if (!Json::parse(line, request, parse_error) || !request.is_object()) {
			reply = err(0, "bad_request", "malformed request: " + parse_error);
		} else {
			request.get("id").as_u64(id);
			std::string method = request.get("method").as_string();
			Json params = request.get("params");
			if (!params.is_object()) params = Json::object();
			bool owned = false;
			for (const auto& member : params.members()) {
				if (member.first.rfind("_control", 0) == 0 || member.first == "_temporal_owner") owned = true;
			}
			bool handled = false;
			if (owned) {
				reply = err(id, "unsupported", "owned temporal control is not implemented by this producer");
			} else {
				reply = local_reply(id, method, params, handled);
				if (!handled) {
					auto job = std::make_shared<Job>();
					job->id = id;
					job->method = method;
					job->params = params;
					{
						std::lock_guard<std::mutex> lock(queue_mutex_);
						queue_.push_back(job);
					}
					queue_cv_.notify_all();
					std::unique_lock<std::mutex> lock(job->mutex);
					while (!job->done && !quit_) {
						if (job->done_cv.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout && !job->done) {
							std::string working = ok(id, [] {
								Json w = Json::object();
								w.set("status", Json("working"));
								return w;
							}());
							working.push_back('\n');
							lock.unlock();
							bool sent = send_all(fd, working.data(), working.size());
							lock.lock();
							if (!sent) return false;
						}
					}
					reply = job->reply;
				}
			}
		}
		reply.push_back('\n');
		if (!send_all(fd, reply.data(), reply.size())) return false;
	}
	return true;
}

// ---- emulator thread --------------------------------------------------------------------------

void Producer::finish(const std::shared_ptr<Job>& job, const std::string& reply) {
	std::lock_guard<std::mutex> lock(job->mutex);
	job->reply = reply;
	job->done = true;
	job->done_cv.notify_all();
}

std::string Producer::handle_line_now(const std::string& line) {
	Json request;
	std::string parse_error;
	if (!Json::parse(line, request, parse_error)) return err(0, "bad_request", parse_error);
	uint64_t id = 0;
	request.get("id").as_u64(id);
	std::string method = request.get("method").as_string();
	Json params = request.get("params");
	if (!params.is_object()) params = Json::object();
	bool handled = false;
	std::string reply = local_reply(id, method, params, handled);
	if (handled) return reply;
	auto job = std::make_shared<Job>();
	job->id = id;
	job->method = method;
	job->params = params;
	std::string immediate = run_job(job);
	if (!immediate.empty()) return immediate;
	// A deferred step: run boundaries inline is the emulator's job; report working.
	return ok(id, [] { Json w = Json::object(); w.set("status", Json("deferred")); return w; }());
}

void Producer::entry() {
	if (!started_) return;
	at_entry_ = true;
	park();
	at_entry_ = false;
}

void Producer::complete_step(const char* status, const char* reason) {
	Json result = Json::object();
	result.set("status", Json(status));
	if (reason) result.set("reason", Json(reason));
	result.set("unit", Json("frames"));
	result.set("requested", Json((unsigned long long)step_requested_));
	result.set("completed", Json((unsigned long long)(step_requested_ - step_remaining_)));
	result.set("count", Json((unsigned long long)(step_requested_ - step_remaining_)));
	result.set("frame_before", Json((unsigned long long)step_frame_before_));
	result.set("frame", Json((unsigned long long)frame_.load()));
	result.set("state", Json("frozen"));
	result.set("was_running", Json(step_was_running_));
	result.set("boundary", Json("frame_boundary"));
	auto job = step_job_;
	step_job_.reset();
	step_remaining_ = 0;
	frozen_ = true;
	stop_epoch_++;
	finish(job, ok(job->id, result));
}

void Producer::boundary() {
	frame_++;
	if (!started_) return;
	if (step_job_) {
		if (step_remaining_ > 0) step_remaining_--;
		if (step_remaining_ == 0) {
			complete_step("completed", nullptr);
		} else if (now_ms() >= step_deadline_ms_) {
			complete_step("interrupted", "host_deadline");
		} else {
			return;
		}
	}
	park();
}

void Producer::park() {
	auto run_guest = [this] {
		if (halted_) {
			backend_.unhalt();
			halted_ = false;
		}
	};
	while (!quit_) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			if (queue_.empty()) {
				if (!frozen_.load() || step_job_) {
					lock.unlock();
					run_guest();
					return;
				}
				if (!halted_) {
					lock.unlock();
					backend_.halt();
					halted_ = true;
					continue;
				}
				queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
				continue;
			}
			job = queue_.front();
			queue_.pop_front();
		}
		if (frozen_.load() && !halted_) {
			backend_.halt();
			halted_ = true;
		}
		execute(job);
		if (step_job_) {  // run frames for the step
			run_guest();
			return;
		}
	}
}

void Producer::execute(const std::shared_ptr<Job>& job) {
	std::string reply;
	try {
		reply = run_job(job);
	} catch (const std::exception& e) {
		reply = err(job->id, "internal_error", e.what());
	} catch (...) {
		reply = err(job->id, "internal_error", "unknown producer exception");
	}
	// A frozen stop is only reported after every guest thread has stopped.
	if (frozen_.load() && !step_job_ && !halted_) {
		backend_.halt();
		halted_ = true;
	}
	if (!reply.empty()) finish(job, reply);
}

static Region* find_region(std::vector<Region>& regions, const std::string& name) {
	for (auto& region : regions)
		if (region.name == name) return &region;
	return nullptr;
}

std::string Producer::run_job(const std::shared_ptr<Job>& job) {
	const uint64_t id = job->id;
	const std::string& method = job->method;
	const Json& p = job->params;
	const char* state = frozen_.load() ? "frozen" : "running";

	if (method == "pause") {
		frozen_ = true;
		stop_epoch_++;
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("state", Json("frozen"));
		r.set("frame", Json((unsigned long long)frame_.load()));
		r.set("boundary", Json(at_entry_ ? "pre_first_instruction" : "frame_boundary"));
		return ok(id, r);
	}
	if (method == "resume") {
		frozen_ = false;
		if (backend_.self_paced() && halted_) {
			backend_.unhalt();
			halted_ = false;
		}
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("state", Json("running"));
		r.set("frame", Json((unsigned long long)frame_.load()));
		return ok(id, r);
	}
	if (method == "step") {
		if (p.get("unit").is_string() && p.get("unit").as_string() != "frames")
			return err(id, "unsupported", "only step(unit=frames) is available");
		uint64_t count = 1;
		if (p.has("frames") && !p.get("frames").as_u64(count)) return err(id, "bad_params", "frames must be an integer");
		if (!p.has("frames") && p.has("count") && !p.get("count").as_u64(count)) return err(id, "bad_params", "count must be an integer");
		if (count < 1 || count > kMaxSyncAdvance)
			return err(id, "bad_params", "frame count must be in 1..=" + std::to_string(kMaxSyncAdvance) + "; split the request");
		step_was_running_ = !frozen_.load();
		if (backend_.self_paced()) {
			if (!halted_) {
				backend_.halt();
				halted_ = true;
			}
			const uint64_t before = backend_.frame_count();
			uint64_t completed = 0;
			std::string error;
			const bool ok_advance = backend_.advance(count, now_ms() + kAdvanceBudgetMs, completed, error);
			frozen_ = true;
			stop_epoch_++;
			frame_ = backend_.frame_count();
			if (!ok_advance && completed == 0) return err(id, "emulator_error", error);
			Json r = Json::object();
			r.set("status", Json(completed == count ? "completed" : "interrupted"));
			if (completed != count) r.set("reason", Json(error.empty() ? "host_deadline" : error));
			r.set("unit", Json("frames"));
			r.set("requested", Json((unsigned long long)count));
			r.set("completed", Json((unsigned long long)completed));
			r.set("count", Json((unsigned long long)completed));
			r.set("frame_before", Json((unsigned long long)before));
			r.set("frame", Json((unsigned long long)frame_.load()));
			r.set("state", Json("frozen"));
			r.set("was_running", Json(step_was_running_));
			r.set("boundary", Json("frame_boundary"));
			return ok(id, r);
		}
		frozen_ = true;
		step_job_ = job;
		step_requested_ = count;
		step_remaining_ = count;
		step_frame_before_ = frame_.load();
		step_deadline_ms_ = now_ms() + kAdvanceBudgetMs;
		return std::string();
	}
	if (method == "set_input") {
		uint64_t port = 0;
		if (p.has("port") && (!p.get("port").as_u64(port) || port != 0))
			return err(id, "bad_params", "this adapter supports only controller port 0");
		const Json& buttons = p.get("buttons");
		if (!buttons.is_null() && !buttons.is_array()) return err(id, "bad_params", "buttons must be an array of names");
		auto names = backend_.button_names();
		uint64_t mask = 0;
		Json applied = Json::array();
		for (const auto& item : buttons.items()) {
			std::string name = item.as_string();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			auto it = std::find(names.begin(), names.end(), name);
			if (it == names.end()) {
				std::string available;
				for (const auto& n : names) available += (available.empty() ? "" : ", ") + n;
				return err(id, "bad_params", "unknown button " + item.dump() + "; available: " + available);
			}
			mask |= 1ull << (it - names.begin());
			applied.push(Json(name));
		}
		if (p.has("pointer") && backend_.has_pointer()) {
			const Json& ptr = p.get("pointer");
			uint64_t x = 0, y = 0;
			ptr.get("x").as_u64(x);
			ptr.get("y").as_u64(y);
			backend_.set_pointer(ptr.get("down").is_bool() ? ptr.get("down").as_bool() : true, (int)x, (int)y);
		}
		held_mask_ = mask;
		override_engaged_ = mask != 0;
		backend_.set_buttons(mask);
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("applied", Json(true));
		char mask_text[32];
		std::snprintf(mask_text, sizeof(mask_text), "0x%llx", (unsigned long long)mask);
		r.set("applied_mask", Json(mask_text));
		r.set("applied_buttons", applied);
		r.set("override_engaged", Json(override_engaged_.load()));
		return ok(id, r);
	}
	if (method == "get_state") {
		std::vector<CpuState> cpus;
		backend_.cpu_state(cpus);
		if (cpus.empty()) return err(id, "unsupported", "no CPU state is available");
		const CpuState* chosen = &cpus[0];
		if (p.get("cpu").is_string()) {
			chosen = nullptr;
			const std::string& want = p.get("cpu").as_string();
			for (const auto& c : cpus) {
				if (c.cpu_id == want || std::find(c.aliases.begin(), c.aliases.end(), want) != c.aliases.end()) { chosen = &c; break; }
			}
			if (!chosen) return err(id, "bad_params", "unknown cpu " + want);
		}
		Json regs = Json::object();
		for (const auto& reg : chosen->registers) regs.set(reg.first, Json((unsigned long long)reg.second));
		Json r = Json::object();
		r.set("cpu", Json(chosen->cpu_id));
		r.set("state", regs);
		r.set("groups_applied", Json::strings({"cpu"}));
		r.set("frame", Json((unsigned long long)frame_.load()));
		r.set("execution_state", Json(state));
		return ok(id, r);
	}
	if (method == "read_memory" || method == "write_memory") {
		Region* region = find_region(backend_.regions(), p.get("memory_type").as_string());
		if (!region) {
			std::string available;
			for (const auto& r : backend_.regions()) available += (available.empty() ? "" : ", ") + r.name;
			return err(id, "bad_params", "unsupported memory_type " + p.get("memory_type").dump() + "; available: " + available);
		}
		uint64_t address = 0;
		if (!p.get("address").as_u64(address)) return err(id, "bad_params", "address is required");
		if (method == "read_memory") {
			uint64_t length = 0;
			if (!p.get("length").as_u64(length) || length == 0 || length > kMaxTransfer)
				return err(id, "bad_params", "length must be in 1..=0x10000");
			if (address > region->size || length > region->size - address)
				return err(id, "bad_params", "address+length exceeds " + region->name + " region size " + std::to_string(region->size));
			std::vector<uint8_t> bytes((size_t)length);
			if (!region->read(address, bytes.data(), bytes.size())) return err(id, "emulator_error", "memory read failed");
			Json r = Json::object();
			r.set("memory_type", Json(region->name));
			r.set("address", Json((unsigned long long)address));
			r.set("length", Json((unsigned long long)length));
			r.set("hex", Json(hex_encode(bytes.data(), bytes.size())));
			r.set("frame", Json((unsigned long long)frame_.load()));
			r.set("execution_state", Json(state));
			return ok(id, r);
		}
		if (!region->writable || !region->write) return err(id, "unsupported", region->name + " is read-only");
		std::vector<uint8_t> bytes;
		if (!hex_decode(p.get("hex").as_string(), bytes) || bytes.size() > kMaxTransfer)
			return err(id, "bad_params", "hex must be a non-empty even-length hexadecimal string of at most 0x10000 bytes");
		if (address > region->size || bytes.size() > region->size - address)
			return err(id, "bad_params", "address+hex length exceeds " + region->name + " region size");
		if (!region->write(address, bytes.data(), bytes.size())) return err(id, "emulator_error", "memory write failed");
		Json r = Json::object();
		r.set("memory_type", Json(region->name));
		r.set("address", Json((unsigned long long)address));
		r.set("written", Json((unsigned long long)bytes.size()));
		r.set("execution_state", Json(state));
		return ok(id, r);
	}
	if (method == "read_memory_batch") {
		if (!has_batch_window()) return err(id, "unsupported", "no side-effect-free memory window is available for batch reads");
		if (!frozen_.load()) return err(id, "not_frozen", "read_memory_batch requires a frozen stop; call pause first");
		const Json& ranges = p.get("ranges");
		if (!ranges.is_array() || ranges.items().empty() || ranges.items().size() > 64)
			return err(id, "bad_params", "ranges must be an array of 1..=64 ranges");
		struct R { Region* region; uint64_t address, length; };
		std::vector<R> parsed;
		uint64_t total = 0;
		for (size_t i = 0; i < ranges.items().size(); i++) {
			const Json& item = ranges.items()[i];
			Region* region = find_region(backend_.regions(), item.get("memory_type").as_string());
			uint64_t address = 0, length = 0;
			if (!region || !region->batch || !item.get("address").as_u64(address) || !item.get("length").as_u64(length)
				|| length == 0 || address > region->size || length > region->size - address)
				return err(id, "bad_params", "range " + std::to_string(i) + " is outside an advertised window");
			total += length;
			parsed.push_back({region, address, length});
		}
		if (total > 65536) return err(id, "bad_params", "ranges exceed max_total_bytes 65536");
		Json reads = Json::array();
		for (size_t i = 0; i < parsed.size(); i++) {
			std::vector<uint8_t> bytes((size_t)parsed[i].length);
			if (!parsed[i].region->read(parsed[i].address, bytes.data(), bytes.size()))
				return err(id, "emulator_error", "memory read failed");
			Json read = Json::object();
			read.set("index", Json((unsigned long long)i));
			read.set("memory_type", Json(parsed[i].region->name));
			read.set("address", Json((unsigned long long)parsed[i].address));
			read.set("length", Json((unsigned long long)parsed[i].length));
			read.set("hex", Json(hex_encode(bytes.data(), bytes.size())));
			reads.push(read);
		}
		std::string epoch = "f" + std::to_string(frame_.load()) + "#s" + std::to_string(stop_epoch_.load());
		Json boundary = Json::object();
		boundary.set("runtime_generation", Json(launch_id_.empty() ? "unmanaged-" + std::to_string(current_pid()) : launch_id_));
		boundary.set("stop_epoch", Json(epoch));
		boundary.set("memory_mapping_epoch", Json(epoch));
		boundary.set("kind", Json(at_entry_ ? "pre_first_instruction" : "frame_boundary"));
		Json clocks = Json::array();
		Json clock = Json::object();
		clock.set("domain", Json("frame"));
		clock.set("value", Json((unsigned long long)frame_.load()));
		clocks.push(clock);
		boundary.set("clocks", clocks);
		Json r = Json::object();
		r.set("state", Json("frozen"));
		r.set("consistency", Json("frozen_boundary"));
		r.set("boundary", boundary);
		r.set("total_bytes", Json((unsigned long long)total));
		r.set("reads", reads);
		return ok(id, r);
	}
	if (method == "screenshot") {
		FrameImage image;
		std::string error;
		if (!backend_.capture(image, error)) return err(id, "no_frame", error.empty() ? "no rendered frame is available" : error);
		if (image.width <= 0 || image.height <= 0 || image.rgb.size() < (size_t)image.width * image.height * 3)
			return err(id, "no_frame", "no rendered frame is available");
		std::vector<uint8_t> png;
		if (!encode_png_rgb(image.rgb.data(), image.width, image.height, png)) return err(id, "io_error", "PNG encoding failed");
		if (png.size() > (kMaxLine - 65536) / 4 * 3) return err(id, "io_error", "PNG exceeds the inline frame limit");
		Json r = Json::object();
		r.set("png_base64", Json(base64_encode(png.data(), png.size())));
		r.set("sha256", Json(sha256_hex(png.data(), png.size())));
		r.set("byte_len", Json((unsigned long long)png.size()));
		r.set("width", Json(image.width));
		r.set("height", Json(image.height));
		r.set("frame", Json((unsigned long long)frame_.load()));
		r.set("state", Json(state));
		r.set("freshness", Json("current"));
		return ok(id, r);
	}
	if (method == "save_state" || method == "load_state") {
		if (!backend_.supports_state()) return err(id, "unsupported", "savestates are not supported by this adapter");
		std::string path = p.get("path").as_string();
		if (path.empty()) return err(id, "bad_params", "path is required");
		std::string error;
		if (method == "save_state") {
			std::vector<uint8_t> payload;
			if (!backend_.save_state(payload, error)) return err(id, "emulator_error", error);
			std::vector<uint8_t> file(kStateMagic, kStateMagic + 8);
			std::string adapter = backend_.adapter();
			file.push_back((uint8_t)adapter.size());
			file.insert(file.end(), adapter.begin(), adapter.end());
			uint64_t frame = frame_.load();
			for (int i = 0; i < 8; i++) file.push_back((uint8_t)(frame >> (8 * i)));
			file.insert(file.end(), payload.begin(), payload.end());
			if (!write_file_atomic(path, file, error)) return err(id, "io_error", error);
			Json r = Json::object();
			r.set("status", Json("completed"));
			r.set("path", Json(path));
			r.set("format", Json(backend_.adapter() + "-emucap-v1"));
			r.set("bytes", Json((unsigned long long)file.size()));
			r.set("sha256", Json(sha256_hex(file.data(), file.size())));
			r.set("frame", Json((unsigned long long)frame));
			r.set("state", Json(state));
			return ok(id, r);
		}
		std::vector<uint8_t> file;
		if (!read_file(path, file, error)) return err(id, "bad_params", error);
		std::string adapter = backend_.adapter();
		size_t header = 8 + 1 + adapter.size() + 8;
		if (file.size() < header || std::memcmp(file.data(), kStateMagic, 8) != 0 || file[8] != adapter.size()
			|| std::memcmp(file.data() + 9, adapter.data(), adapter.size()) != 0)
			return err(id, "bad_params", "not a " + adapter + " emucap savestate");
		uint64_t frame = 0;
		for (int i = 0; i < 8; i++) frame |= (uint64_t)file[9 + adapter.size() + i] << (8 * i);
		std::vector<uint8_t> payload(file.begin() + (long)header, file.end());
		uint64_t before = frame_.load();
		if (!backend_.load_state(payload, error)) return err(id, "emulator_error", error);
		frame_ = frame;
		frozen_ = true;
		stop_epoch_++;
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("path", Json(path));
		r.set("sha256", Json(sha256_hex(file.data(), file.size())));
		r.set("state", Json("frozen"));
		r.set("frame_before", Json((unsigned long long)before));
		r.set("frame", Json((unsigned long long)frame));
		return ok(id, r);
	}
	if (method == "reset") {
		if (!backend_.supports_reset()) return err(id, "unsupported", "reset is not supported");
		std::string error;
		if (!backend_.reset(error)) return err(id, "emulator_error", error);
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("reset", Json("completed"));
		r.set("state", Json(state));
		return ok(id, r);
	}
	if (method == "list_breakpoints") {
		Json r = Json::object();
		r.set("breakpoints", Json::array());
		return ok(id, r);
	}
	if (method == "clear_all_breakpoints") {
		Json r = Json::object();
		r.set("status", Json("completed"));
		r.set("cleared", Json(0));
		return ok(id, r);
	}
	if (method == "poll_events") {
		Json r = Json::object();
		r.set("events", Json::array());
		r.set("dropped", Json(0));
		return ok(id, r);
	}
	return err(id, "unknown_method", method);
}

}  // namespace emucap_producer
