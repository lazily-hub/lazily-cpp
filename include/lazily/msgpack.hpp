#ifndef LAZILY_MSGPACK_HPP
#define LAZILY_MSGPACK_HPP

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lazily {

// Minimal, zero-dependency MessagePack packer/unpacker for the lazily wire codec.
// Implements the subset lazily needs: nil, bool, int (all forms), str, bin,
// array, map. Float/ext are not produced (lazily wire types are integer/string/
// bytes only) but are skipped on read for forward compatibility.
//
// Reference: https://github.com/msgpack/msgpack/blob/master/spec.md

// ─────────────────────────────────────────────────────────────────────────────
// Packer — writes the smallest valid encoding of each value.
// ─────────────────────────────────────────────────────────────────────────────
class MsgPacker {
 public:
  const std::vector<uint8_t>& bytes() const { return buf_; }
  std::vector<uint8_t> take() && { return std::move(buf_); }

  // Pre-allocate capacity for the output buffer. Call once at the top of a
  // pack sequence whose approximate size is known (e.g. encode_snapshot over N
  // nodes) so per-push_back capacity checks do not trigger logarithmic
  // re-growths over the course of the message (#lzcppreservehint).
  void reserve_hint(size_t n) { buf_.reserve(n); }

  void nil() { buf_.push_back(0xc0); }
  void boolean(bool v) { buf_.push_back(v ? 0xc3 : 0xc2); }

  void i64(int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    if (v >= 0) {
      if (u <= 0x7f) {
        buf_.push_back(static_cast<uint8_t>(u));
      } else if (u <= 0xff) {
        buf_.push_back(0xcc);
        push_u8(u);
      } else if (u <= 0xffff) {
        buf_.push_back(0xcd);
        push_u16(u);
      } else if (u <= 0xffffffffu) {
        buf_.push_back(0xce);
        push_u32(u);
      } else {
        buf_.push_back(0xcf);
        push_u64(u);
      }
    } else {
      if (v >= -32) {
        buf_.push_back(static_cast<uint8_t>(v));  // negative fixint
      } else if (v >= -128) {
        buf_.push_back(0xd0);
        push_u8(static_cast<uint64_t>(v) & 0xff);
      } else if (v >= -32768) {
        buf_.push_back(0xd1);
        push_u16(static_cast<uint64_t>(v) & 0xffff);
      } else if (v >= -2147483648LL) {
        buf_.push_back(0xd2);
        push_u32(static_cast<uint64_t>(v) & 0xffffffffu);
      } else {
        buf_.push_back(0xd3);
        push_u64(static_cast<uint64_t>(v));
      }
    }
  }

  void str(std::string_view s) {
    size_t n = s.size();
    if (n <= 31) {
      buf_.push_back(0xa0 | static_cast<uint8_t>(n));
    } else if (n <= 0xff) {
      buf_.push_back(0xd9);
      push_u8(n);
    } else if (n <= 0xffff) {
      buf_.push_back(0xda);
      push_u16(n);
    } else {
      buf_.push_back(0xdb);
      push_u32(n);
    }
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void bin(const uint8_t* data, size_t n) {
    if (n <= 0xff) {
      buf_.push_back(0xc4);
      push_u8(n);
    } else if (n <= 0xffff) {
      buf_.push_back(0xc5);
      push_u16(n);
    } else {
      buf_.push_back(0xc6);
      push_u32(n);
    }
    if (n) buf_.insert(buf_.end(), data, data + n);
  }
  void bin(const std::vector<uint8_t>& b) { bin(b.data(), b.size()); }

  void array_header(uint32_t n) {
    if (n <= 15) {
      buf_.push_back(0x90 | static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
      buf_.push_back(0xdc);
      push_u16(n);
    } else {
      buf_.push_back(0xdd);
      push_u32(n);
    }
  }
  void map_header(uint32_t n) {
    if (n <= 15) {
      buf_.push_back(0x80 | static_cast<uint8_t>(n));
    } else if (n <= 0xffff) {
      buf_.push_back(0xde);
      push_u16(n);
    } else {
      buf_.push_back(0xdf);
      push_u32(n);
    }
  }

 private:
  std::vector<uint8_t> buf_;
  void push_u8(uint64_t v) { buf_.push_back(static_cast<uint8_t>(v & 0xff)); }
  void push_u16(uint64_t v) {
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    buf_.push_back(static_cast<uint8_t>(v & 0xff));
  }
  void push_u32(uint64_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      buf_.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
  }
  void push_u64(uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8)
      buf_.push_back(static_cast<uint8_t>((v >> shift) & 0xff));
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Unpacker — reads from a byte span; accepts any valid encoding of each type.
// Throws std::runtime_error on truncation / unsupported tags.
// ─────────────────────────────────────────────────────────────────────────────
class MsgUnpacker {
 public:
  MsgUnpacker(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}
  explicit MsgUnpacker(const std::vector<uint8_t>& b)
      : p_(b.data()), end_(b.data() + b.size()) {}

  bool eof() const { return p_ >= end_; }
  size_t remaining() const { return end_ - p_; }

  // Peek the next value's wire category without consuming.
  enum class Kind {
    Nil, Bool, Int, Str, Bin, Array, Map, Float, Ext, Other
  };
  Kind peek_kind() const {
    require(1);
    uint8_t b = *p_;
    if (b <= 0x7f) return Kind::Int;            // positive fixint
    if (b <= 0x8f) return Kind::Map;            // fixmap
    if (b <= 0x9f) return Kind::Array;          // fixarray
    if (b <= 0xbf) return Kind::Str;            // fixstr
    if (b >= 0xe0) return Kind::Int;            // negative fixint
    switch (b) {
      case 0xc0: return Kind::Nil;
      case 0xc2: case 0xc3: return Kind::Bool;
      case 0xc4: case 0xc5: case 0xc6: return Kind::Bin;
      case 0xca: case 0xcb: return Kind::Float;
      case 0xcc: case 0xcd: case 0xce: case 0xcf:
      case 0xd0: case 0xd1: case 0xd2: case 0xd3: return Kind::Int;
      case 0xd9: case 0xda: case 0xdb: return Kind::Str;
      case 0xdc: case 0xdd: return Kind::Array;
      case 0xde: case 0xdf: return Kind::Map;
      default: return Kind::Other;  // ext / fixext — lazily never produces these
    }
  }

  void expect_nil() {
    require(1);
    if (*p_ != 0xc0) throw std::runtime_error("msgpack: expected nil");
    ++p_;
  }
  bool read_bool() {
    require(1);
    uint8_t b = *p_++;
    if (b == 0xc3) return true;
    if (b == 0xc2) return false;
    throw std::runtime_error("msgpack: expected bool");
  }

  int64_t read_i64() {
    require(1);
    uint8_t b = *p_++;
    if (b <= 0x7f) return b;                       // positive fixint
    if (b >= 0xe0) return static_cast<int8_t>(b);  // negative fixint
    switch (b) {
      case 0xcc: return read_u8();
      case 0xcd: return read_u16();
      case 0xce: return read_u32();
      case 0xcf: return static_cast<int64_t>(read_u64());
      case 0xd0: return static_cast<int8_t>(read_u8());
      case 0xd1: return static_cast<int16_t>(read_u16());
      case 0xd2: return static_cast<int32_t>(read_u32());
      case 0xd3: return static_cast<int64_t>(read_u64());
      default: throw std::runtime_error("msgpack: expected int");
    }
  }

  std::string read_str() {
    require(1);
    uint8_t b = *p_++;
    size_t n;
    if ((b & 0xe0) == 0xa0) {
      n = b & 0x1f;  // fixstr
    } else if (b == 0xd9) {
      n = read_u8();
    } else if (b == 0xda) {
      n = read_u16();
    } else if (b == 0xdb) {
      n = read_u32();
    } else {
      throw std::runtime_error("msgpack: expected str");
    }
    require(n);
    std::string out(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return out;
  }

  // Zero-copy string read — returns a view into the unpacker's buffer.
  // Safe whenever the buffer outlives the consumer (e.g. map-key dispatch
  // during decode, where keys are matched against literals and discarded).
  // Removes the per-key std::string allocation + copy that read_str() pays
  // (#lzcppstrview).
  std::string_view read_str_view() {
    require(1);
    uint8_t b = *p_++;
    size_t n;
    if ((b & 0xe0) == 0xa0) {
      n = b & 0x1f;  // fixstr
    } else if (b == 0xd9) {
      n = read_u8();
    } else if (b == 0xda) {
      n = read_u16();
    } else if (b == 0xdb) {
      n = read_u32();
    } else {
      throw std::runtime_error("msgpack: expected str");
    }
    require(n);
    std::string_view out(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return out;
  }

  std::vector<uint8_t> read_bin() {
    require(1);
    uint8_t b = *p_++;
    size_t n;
    if (b == 0xc4) {
      n = read_u8();
    } else if (b == 0xc5) {
      n = read_u16();
    } else if (b == 0xc6) {
      n = read_u32();
    } else {
      throw std::runtime_error("msgpack: expected bin");
    }
    require(n);
    std::vector<uint8_t> out(p_, p_ + n);
    p_ += n;
    return out;
  }

  uint32_t read_array_header() {
    require(1);
    uint8_t b = *p_++;
    if ((b & 0xf0) == 0x90) return b & 0x0f;  // fixarray
    if (b == 0xdc) return read_u16();
    if (b == 0xdd) return read_u32();
    throw std::runtime_error("msgpack: expected array");
  }
  uint32_t read_map_header() {
    require(1);
    uint8_t b = *p_++;
    if ((b & 0xf0) == 0x80) return b & 0x0f;  // fixmap
    if (b == 0xde) return read_u16();
    if (b == 0xdf) return read_u32();
    throw std::runtime_error("msgpack: expected map");
  }

  // Skip any value (used to ignore unknown map keys for forward compatibility).
  void skip() {
    switch (peek_kind()) {
      case Kind::Nil: expect_nil(); break;
      case Kind::Bool: (void)read_bool(); break;
      case Kind::Int: (void)read_i64(); break;
      case Kind::Str: (void)read_str(); break;
      case Kind::Bin: (void)read_bin(); break;
      case Kind::Array: {
        uint32_t n = read_array_header();
        for (uint32_t i = 0; i < n; ++i) skip();
        break;
      }
      case Kind::Map: {
        uint32_t n = read_map_header();
        for (uint32_t i = 0; i < n; ++i) { skip(); skip(); }
        break;
      }
      default: throw std::runtime_error("msgpack: cannot skip unsupported value");
    }
  }

 private:
  const uint8_t *p_, *end_;
  void require(size_t n) const {
    if (static_cast<size_t>(end_ - p_) < n)
      throw std::runtime_error("msgpack: truncated");
  }
  uint8_t read_u8() {
    require(1);
    return *p_++;
  }
  uint16_t read_u16() {
    require(2);
    uint16_t v = (uint16_t(p_[0]) << 8) | p_[1];
    p_ += 2;
    return v;
  }
  uint32_t read_u32() {
    require(4);
    uint32_t v = (uint32_t(p_[0]) << 24) | (uint32_t(p_[1]) << 16) |
                 (uint32_t(p_[2]) << 8) | p_[3];
    p_ += 4;
    return v;
  }
  uint64_t read_u64() {
    require(8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p_[i];
    p_ += 8;
    return v;
  }
};

}  // namespace lazily

#endif  // LAZILY_MSGPACK_HPP
