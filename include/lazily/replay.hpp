#ifndef LAZILY_REPLAY_HPP
#define LAZILY_REPLAY_HPP

// Replay-equivalence proof for a reactive graph (`#lzreplaycpp`).
//
// The contract is `lazily-spec/docs/replay-equivalence.md`:
//
//     Given the same event log, a REBUILT graph observes the same values at
//     every checkpoint. Any deviation is a defect in the graph, not a
//     tolerance.
//
// It is a MAY row — a verification facility, not a runtime feature — and it is
// what lets a host that re-executes your code from an event log (Temporal.io
// workflow replay, an event-sourced aggregate, a deterministic simulation) be
// given a reactive graph at all.
//
// Three obligations, taken from `tsift`'s rule that a cached excerpt records a
// body hash and revalidates it against the source bytes before it is returned:
//
//   1. `ReplayFingerprint` carries the digest of the log that produced it, and
//      `verify`/`check` revalidate that binding BEFORE comparing any observed
//      value. A stale fingerprint raises `ReplayLogMismatchError` and is never
//      compared — two different logs can settle to the same final values
//      (`[+1,+2,+3]` and `[+3,+2,+1]` both sum to 6), so a value-only
//      comparison would certify a fingerprint that proves nothing about the log
//      in front of it.
//   2. A divergence is reported at the FIRST checkpoint where the values
//      parted, naming the cell label — not merely at the final state where the
//      defect is still visible. `stride` is part of the fingerprint, because
//      equal log digest PLUS equal stride is what makes two checkpoint
//      sequences comparable at all; a mismatch is `ReplayStrideMismatchError`,
//      a distinct type from the log mismatch because it is a distinct fault.
//   3. The observation encoding is canonical — type-tagged and length-framed —
//      or it fails. A value with no defined encoding raises
//      `ReplayEncodingError` rather than falling back on a host default
//      rendering, which in C++ means an address-bearing pointer or a mangled
//      type name and would report a FALSE divergence on every run.
//
// ## Why this binding stores BYTES where lazily-py stores a hash
//
// lazily-py fingerprints with BLAKE2b-256 because `hashlib` is in its standard
// library. C++ has no standard-library hash suitable here — `std::hash` is
// neither stable across runs (libstdc++ seeds nothing, but the standard permits
// it and `std::hash<std::string>` is explicitly not required to be stable) nor
// collision-resistant, and it is defined over values rather than over canonical
// bytes. Vendoring a cryptographic hash for an OPTIONAL verification facility
// would buy a maintained dependency for nothing.
//
// So this binding takes the degenerate strongest choice: the "digest" of a
// value IS its exact canonical byte string. The spec deliberately leaves both
// the hash and the byte layout binding-chosen — "fingerprints are pinned next
// to a test in one language and are not exchanged between bindings, so there is
// nothing to agree on at the byte level" — and what a binding MUST agree on is
// the equality CLASSES, which exact bytes satisfy with a collision probability
// of exactly zero. At conformance and unit-test sizes the bytes are shorter
// than the 32-byte digest they would hash to.
//
// `canonical_digest` is kept as a name so the family vocabulary reads the same
// across bindings; here it is the identity function over `canonical_bytes`.
// `replay_hex` renders a digest for a message or for pinning next to a test.
//
// ## wasm
//
// The closure of this header is the C++17 standard library — no threads, no
// files, no POSIX — so it builds in the `core` wasm tier and
// `tests/test_replay_conformance` is registered there (`wasm-tiers.conf`). It
// is deliberately NOT included from `lazily/core.hpp`: the narrow-include
// contract names the reactive kernel and the structures built on it, and a
// verification facility is not part of that closure. Include it directly.
//
// Failure is signalled by THROWING, and under Emscripten `emcc` defaults to
// `-fignore-exceptions`, which compiles `throw` into an abort and drops every
// `catch`. That default would turn every refusal path here into
// `Aborted(undefined)` — a red run, not a false green, but an unreadable one.
// `tests/wasm.cmake` passes `-fexceptions` to every wasm target for exactly
// this reason (several other suites already assert error paths), so the
// obligations above are observed under wasm the same way they are natively.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lazily {

// The checkpoint sequence number for the state before any event was applied.
inline constexpr std::int64_t kReplayInitialSeq = -1;

// -- errors -------------------------------------------------------------------

// A replay-equivalence proof could not be completed as stated.
class ReplayProofError : public std::runtime_error {
public:
  explicit ReplayProofError(const std::string& what) : std::runtime_error(what) {}
};

// A value has no canonical byte encoding, so it cannot be fingerprinted.
//
// Thrown instead of falling back on a host default rendering. C++ has no
// universal `repr`, and every candidate — a pointer value, `typeid().name()`,
// an uninitialised-padding `memcpy` — either embeds an address or reads bytes
// the object model does not define. Such a fallback reports a FALSE divergence
// on every run: the exact failure a replay proof exists to make impossible,
// arriving as a flaky test instead of a real one.
class ReplayEncodingError : public ReplayProofError {
public:
  explicit ReplayEncodingError(const std::string& what) : ReplayProofError(what) {}
};

// The fingerprint was recorded against a different event log.
//
// The tsift rule: revalidate the recorded digest against the source bytes and
// deterministically suppress the cached answer when they disagree. A stale
// fingerprint is never compared, so it can neither pass by coincidence nor be
// misreported as a value divergence.
class ReplayLogMismatchError : public ReplayProofError {
public:
  ReplayLogMismatchError(std::string expected_digest, std::string actual_digest,
                         const std::string& message)
      : ReplayProofError(message), expected_digest_(std::move(expected_digest)),
        actual_digest_(std::move(actual_digest)) {}

  const std::string& expected_digest() const { return expected_digest_; }
  const std::string& actual_digest() const { return actual_digest_; }

private:
  std::string expected_digest_;
  std::string actual_digest_;
};

// The fingerprint was recorded at a different checkpoint stride.
//
// A separate type from `ReplayLogMismatchError` because it is a separate fault:
// the log is the right one, but the two checkpoint sequences were never
// comparable. Distinct so a driver routes on the TYPE rather than on a message
// string (mirrors lazily-py's `ReplayStrideMismatchError`).
class ReplayStrideMismatchError : public ReplayProofError {
public:
  ReplayStrideMismatchError(int expected_stride, int actual_stride, const std::string& message)
      : ReplayProofError(message), expected_stride_(expected_stride),
        actual_stride_(actual_stride) {}

  int expected_stride() const { return expected_stride_; }
  int actual_stride() const { return actual_stride_; }

private:
  int expected_stride_;
  int actual_stride_;
};

// -- the canonical value ------------------------------------------------------

// A value an observation can carry, with a defined canonical encoding.
//
// C++ has no dynamic value type, so the encoding's domain is stated as a type
// rather than discovered by reflection. `Opaque` is a first-class member of it:
// a caller who observes something outside the domain gets a loud
// `ReplayEncodingError` naming the path, not a silent fallback.
//
// Children live in one flat `items_` vector — `Map` interleaves key, value —
// so the type stays self-referential without a heap indirection per node and
// without instantiating a standard-library template over an incomplete type.
class ReplayValue {
public:
  enum class Kind { Null, Bool, Int, Float, Str, Bytes, Seq, Set, Map, Record, Opaque };

  ReplayValue() = default;

  static ReplayValue null() { return ReplayValue(); }

  static ReplayValue boolean(bool value) {
    ReplayValue out;
    out.kind_ = Kind::Bool;
    out.bool_ = value;
    return out;
  }

  static ReplayValue integer(std::int64_t value) {
    ReplayValue out;
    out.kind_ = Kind::Int;
    out.int_ = value;
    return out;
  }

  static ReplayValue floating(double value) {
    ReplayValue out;
    out.kind_ = Kind::Float;
    out.float_ = value;
    return out;
  }

  static ReplayValue text(std::string value) {
    ReplayValue out;
    out.kind_ = Kind::Str;
    out.text_ = std::move(value);
    return out;
  }

  static ReplayValue bytes(std::string value) {
    ReplayValue out;
    out.kind_ = Kind::Bytes;
    out.text_ = std::move(value);
    return out;
  }

  static ReplayValue seq(std::vector<ReplayValue> items) {
    ReplayValue out;
    out.kind_ = Kind::Seq;
    out.items_ = std::move(items);
    return out;
  }

  static ReplayValue set(std::vector<ReplayValue> items) {
    ReplayValue out;
    out.kind_ = Kind::Set;
    out.items_ = std::move(items);
    return out;
  }

  // Entries are (key, value) pairs. Insertion order is not part of the value;
  // the encoder orders them by their own encoded bytes.
  static ReplayValue map(std::vector<std::pair<ReplayValue, ReplayValue>> entries) {
    ReplayValue out;
    out.kind_ = Kind::Map;
    out.items_.reserve(entries.size() * 2);
    for (auto& entry : entries) {
      out.items_.push_back(std::move(entry.first));
      out.items_.push_back(std::move(entry.second));
    }
    return out;
  }

  // A named, ordered aggregate — the analogue of lazily-py's dataclass arm.
  // Field order IS part of the value, unlike `map`.
  static ReplayValue record(std::string type_name,
                            std::vector<std::pair<std::string, ReplayValue>> fields) {
    ReplayValue out;
    out.kind_ = Kind::Record;
    out.text_ = std::move(type_name);
    out.names_.reserve(fields.size());
    out.items_.reserve(fields.size());
    for (auto& field : fields) {
      out.names_.push_back(std::move(field.first));
      out.items_.push_back(std::move(field.second));
    }
    return out;
  }

  // A value the encoding does not define. `type_name` is for the error message
  // only and never reaches an encoding, so naming it costs no stability.
  static ReplayValue opaque(std::string type_name) {
    ReplayValue out;
    out.kind_ = Kind::Opaque;
    out.text_ = std::move(type_name);
    return out;
  }

  Kind kind() const { return kind_; }
  bool boolean_value() const { return bool_; }
  std::int64_t int_value() const { return int_; }
  double float_value() const { return float_; }
  const std::string& text_value() const { return text_; }
  const std::vector<ReplayValue>& items() const { return items_; }
  const std::vector<std::string>& field_names() const { return names_; }

private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  std::int64_t int_ = 0;
  double float_ = 0;
  std::string text_;
  std::vector<std::string> names_;
  std::vector<ReplayValue> items_;
};

// -- the canonical encoding ---------------------------------------------------

namespace replay_detail {

// tag + decimal(len(body)) + ':' + body. The length prefix is what makes
// `["a","bc"]` and `["ab","c"]` different values: concatenating member
// encodings without a frame makes them identical, and a harness that cannot
// tell them apart certifies a graph that reshaped its own output.
inline void frame(char tag, const std::string& body, std::string& out) {
  out += tag;
  out += std::to_string(body.size());
  out += ':';
  out += body;
}

// The exact IEEE-754 bit pattern, so the encoding round-trips every double and
// does not fold distinct NaN payloads (or +0.0 and -0.0) together the way a
// shortest-round-trip rendering would.
inline std::string float_bits_hex(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  static const char* const kDigits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kDigits[bits & 0xF];
    bits >>= 4;
  }
  return out;
}

// Returns false and fills `error` when the value has no defined encoding, so
// the recursion reports the PATH to the offending member rather than the root.
inline bool encode(const ReplayValue& value, const std::string& path, std::string& out,
                   std::string& error) {
  switch (value.kind()) {
  case ReplayValue::Kind::Null:
    out += "n0:";
    return true;
  case ReplayValue::Kind::Bool:
    out += value.boolean_value() ? "b1:1" : "b1:0";
    return true;
  case ReplayValue::Kind::Int:
    frame('i', std::to_string(value.int_value()), out);
    return true;
  case ReplayValue::Kind::Float:
    frame('f', float_bits_hex(value.float_value()), out);
    return true;
  case ReplayValue::Kind::Str:
    frame('s', value.text_value(), out);
    return true;
  case ReplayValue::Kind::Bytes:
    frame('y', value.text_value(), out);
    return true;
  case ReplayValue::Kind::Seq: {
    // Member order IS part of the value.
    std::string body;
    for (std::size_t i = 0; i < value.items().size(); ++i) {
      if (!encode(value.items()[i], path + "[" + std::to_string(i) + "]", body, error))
        return false;
    }
    frame('l', body, out);
    return true;
  }
  case ReplayValue::Kind::Set: {
    // Iteration order is NOT part of the value: order members by their own
    // encoded bytes, which is a total order over the domain and needs no
    // cross-type comparison operator.
    std::vector<std::string> members;
    members.reserve(value.items().size());
    for (const auto& item : value.items()) {
      std::string member;
      if (!encode(item, path + "{}", member, error)) return false;
      members.push_back(std::move(member));
    }
    std::sort(members.begin(), members.end());
    std::string body;
    for (const auto& member : members)
      body += member;
    frame('t', body, out);
    return true;
  }
  case ReplayValue::Kind::Map: {
    // Insertion order is NOT part of the value. Sorting the CONCATENATED
    // key+value encodings (rather than the keys alone) keeps mixed-type keys
    // orderable without a comparison operator between them.
    std::vector<std::string> entries;
    entries.reserve(value.items().size() / 2);
    for (std::size_t i = 0; i + 1 < value.items().size(); i += 2) {
      std::string entry;
      if (!encode(value.items()[i], path + "[key]", entry, error)) return false;
      if (!encode(value.items()[i + 1], path + "[value]", entry, error)) return false;
      entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end());
    std::string body;
    for (const auto& entry : entries)
      body += entry;
    frame('m', body, out);
    return true;
  }
  case ReplayValue::Kind::Record: {
    std::string body;
    frame('s', value.text_value(), body);
    for (std::size_t i = 0; i < value.items().size(); ++i) {
      frame('s', value.field_names()[i], body);
      if (!encode(value.items()[i], path + "." + value.field_names()[i], body, error)) return false;
    }
    frame('d', body, out);
    return true;
  }
  case ReplayValue::Kind::Opaque:
    break;
  }
  error = path + ": '" + value.text_value() +
          "' has no canonical encoding; observe a plain value, a record, or a "
          "map/sequence/set of them instead. Falling back on a host rendering would embed an "
          "address or a mangled type name and report a false divergence on every run.";
  return false;
}

} // namespace replay_detail

// Encode `value` to type-tagged, order-stable bytes.
//
// Map and set members are ordered by their own encoded bytes, so insertion and
// iteration order do not change the result. Every frame is length-prefixed and
// type-tagged, so `"1"`, `1`, `1.0` and `true` encode differently and no
// concatenation of members can be confused for another. Anything without a
// defined encoding throws `ReplayEncodingError`.
inline std::string canonical_bytes(const ReplayValue& value) {
  std::string out;
  std::string error;
  if (!replay_detail::encode(value, "value", out, error)) throw ReplayEncodingError(error);
  return out;
}

// The digest of `value`. In this binding a digest IS the canonical byte string
// — see the header comment on why no hash is vendored. Kept under the family's
// name so a reader moving between bindings finds the same vocabulary.
inline std::string canonical_digest(const ReplayValue& value) { return canonical_bytes(value); }

// Render a digest for a message or for pinning next to a test.
inline std::string replay_hex(const std::string& digest) {
  static const char* const kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const char byte : digest) {
    const auto value = static_cast<unsigned char>(byte);
    out += kDigits[value >> 4];
    out += kDigits[value & 0xF];
  }
  return out;
}

// A bounded, structural rendering for a failure message.
//
// Never part of a digest, and deliberately not the encoding: it exists so a
// divergence report says what was observed. It renders structure rather than
// identity, so it carries no address even for an opaque value.
inline std::string replay_preview(const ReplayValue& value, std::size_t limit = 120);

namespace replay_detail {

inline void render(const ReplayValue& value, std::string& out) {
  switch (value.kind()) {
  case ReplayValue::Kind::Null:
    out += "null";
    return;
  case ReplayValue::Kind::Bool:
    out += value.boolean_value() ? "true" : "false";
    return;
  case ReplayValue::Kind::Int:
    out += std::to_string(value.int_value());
    return;
  case ReplayValue::Kind::Float:
    out += "float(0x" + float_bits_hex(value.float_value()) + ")";
    return;
  case ReplayValue::Kind::Str:
    out += '"' + value.text_value() + '"';
    return;
  case ReplayValue::Kind::Bytes:
    out += "0x" + replay_hex(value.text_value());
    return;
  case ReplayValue::Kind::Seq:
  case ReplayValue::Kind::Set: {
    out += value.kind() == ReplayValue::Kind::Seq ? '[' : '{';
    for (std::size_t i = 0; i < value.items().size(); ++i) {
      if (i != 0) out += ", ";
      render(value.items()[i], out);
    }
    out += value.kind() == ReplayValue::Kind::Seq ? ']' : '}';
    return;
  }
  case ReplayValue::Kind::Map: {
    out += '{';
    for (std::size_t i = 0; i + 1 < value.items().size(); i += 2) {
      if (i != 0) out += ", ";
      render(value.items()[i], out);
      out += ": ";
      render(value.items()[i + 1], out);
    }
    out += '}';
    return;
  }
  case ReplayValue::Kind::Record: {
    out += value.text_value();
    out += '(';
    for (std::size_t i = 0; i < value.items().size(); ++i) {
      if (i != 0) out += ", ";
      out += value.field_names()[i];
      out += '=';
      render(value.items()[i], out);
    }
    out += ')';
    return;
  }
  case ReplayValue::Kind::Opaque:
    out += "<opaque " + value.text_value() + ">";
    return;
  }
}

} // namespace replay_detail

inline std::string replay_preview(const ReplayValue& value, std::size_t limit) {
  std::string out;
  replay_detail::render(value, out);
  if (limit != 0 && out.size() > limit) {
    out.resize(limit - 3);
    out += "...";
  }
  return out;
}

// -- the log ------------------------------------------------------------------

// One entry of an ordered event log.
struct ReplayEvent {
  std::int64_t seq = 0;
  std::string name;
  ReplayValue payload;

  ReplayEvent() = default;
  ReplayEvent(std::int64_t seq_, std::string name_, ReplayValue payload_ = ReplayValue::null())
      : seq(seq_), name(std::move(name_)), payload(std::move(payload_)) {}

  ReplayValue to_value() const {
    return ReplayValue::record("ReplayEvent", {{"seq", ReplayValue::integer(seq)},
                                               {"name", ReplayValue::text(name)},
                                               {"payload", payload}});
  }
};

// An ordered event log with a digest over its canonical bytes.
//
// Sequence numbers must strictly increase; they do NOT have to be contiguous,
// because an ack-truncated durable outbox replays real epochs and renumbering
// them would hide a truncated prefix the log digest otherwise catches.
class ReplayLog {
public:
  explicit ReplayLog(std::vector<ReplayEvent> events) : events_(std::move(events)) {
    bool have_previous = false;
    std::int64_t previous = 0;
    for (const auto& event : events_) {
      if (event.seq < 0)
        throw std::invalid_argument("event seq must be non-negative, got " +
                                    std::to_string(event.seq));
      if (event.name.empty()) throw std::invalid_argument("event name must be non-empty");
      if (have_previous && event.seq <= previous)
        throw std::invalid_argument("event log must be strictly increasing in seq, got " +
                                    std::to_string(event.seq) + " after " +
                                    std::to_string(previous));
      previous = event.seq;
      have_previous = true;
    }
    std::vector<ReplayValue> encoded;
    encoded.reserve(events_.size());
    for (const auto& event : events_)
      encoded.push_back(event.to_value());
    digest_ = canonical_digest(ReplayValue::seq(std::move(encoded)));
  }

  // A log from `(name, payload)` pairs, numbered `0..n-1`.
  static ReplayLog from_records(const std::vector<std::pair<std::string, ReplayValue>>& records) {
    std::vector<ReplayEvent> events;
    events.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i)
      events.emplace_back(static_cast<std::int64_t>(i), records[i].first, records[i].second);
    return ReplayLog(std::move(events));
  }

  const std::vector<ReplayEvent>& events() const { return events_; }
  const std::string& digest() const { return digest_; }
  std::size_t size() const { return events_.size(); }
  bool empty() const { return events_.empty(); }

private:
  std::vector<ReplayEvent> events_;
  std::string digest_;
};

// -- the fingerprint ----------------------------------------------------------

// Per-cell digests observed after applying events through `seq`.
//
// `seq` is `kReplayInitialSeq` for the state before any event was applied.
struct ReplayCheckpoint {
  std::int64_t seq = kReplayInitialSeq;
  std::vector<std::pair<std::string, std::string>> cells; // label -> digest, sorted by label

  std::map<std::string, std::string> as_map() const {
    return std::map<std::string, std::string>(cells.begin(), cells.end());
  }
};

// The labelled values one checkpoint covers.
using ReplayObservation = std::map<std::string, ReplayValue>;

inline ReplayCheckpoint replay_checkpoint_of(std::int64_t seq, const ReplayObservation& observed) {
  ReplayCheckpoint checkpoint;
  checkpoint.seq = seq;
  checkpoint.cells.reserve(observed.size());
  // `ReplayObservation` is already ordered by label, so the cells are too.
  for (const auto& entry : observed)
    checkpoint.cells.emplace_back(entry.first, canonical_digest(entry.second));
  return checkpoint;
}

// A recorded, log-bound observation of a replayed graph.
class ReplayFingerprint {
public:
  ReplayFingerprint(std::string log_digest, int stride, std::vector<ReplayCheckpoint> checkpoints)
      : log_digest_(std::move(log_digest)), stride_(stride), checkpoints_(std::move(checkpoints)) {
    if (stride_ < 1)
      throw std::invalid_argument("stride must be >= 1, got " + std::to_string(stride_));
    if (checkpoints_.empty())
      throw std::invalid_argument("a fingerprint needs at least the initial checkpoint");
    std::vector<ReplayValue> encoded;
    encoded.reserve(checkpoints_.size());
    for (const auto& checkpoint : checkpoints_) {
      std::vector<ReplayValue> cells;
      cells.reserve(checkpoint.cells.size());
      for (const auto& cell : checkpoint.cells)
        cells.push_back(
            ReplayValue::seq({ReplayValue::text(cell.first), ReplayValue::bytes(cell.second)}));
      encoded.push_back(
          ReplayValue::record("ReplayCheckpoint", {{"seq", ReplayValue::integer(checkpoint.seq)},
                                                   {"cells", ReplayValue::seq(std::move(cells))}}));
    }
    digest_ = canonical_digest(ReplayValue::record(
        "ReplayFingerprint", {{"log_digest", ReplayValue::bytes(log_digest_)},
                              {"stride", ReplayValue::integer(stride_)},
                              {"checkpoints", ReplayValue::seq(std::move(encoded))}}));
  }

  const std::string& log_digest() const { return log_digest_; }
  int stride() const { return stride_; }
  const std::vector<ReplayCheckpoint>& checkpoints() const { return checkpoints_; }

  // The last checkpoint — the end state of the replay.
  const ReplayCheckpoint& final_checkpoint() const { return checkpoints_.back(); }

  // The digest of the fingerprint itself, for pinning it next to a test.
  const std::string& digest() const { return digest_; }

  bool operator==(const ReplayFingerprint& other) const { return digest_ == other.digest_; }
  bool operator!=(const ReplayFingerprint& other) const { return !(*this == other); }

private:
  std::string log_digest_;
  int stride_;
  std::vector<ReplayCheckpoint> checkpoints_;
  std::string digest_;
};

// -- divergence ---------------------------------------------------------------

enum class ReplayDivergenceKind { Value, Missing, Unexpected };

inline const char* replay_divergence_kind_name(ReplayDivergenceKind kind) {
  switch (kind) {
  case ReplayDivergenceKind::Value:
    return "value";
  case ReplayDivergenceKind::Missing:
    return "missing";
  case ReplayDivergenceKind::Unexpected:
    return "unexpected";
  }
  return "value";
}

// One cell that did not replay to its recorded digest.
struct ReplayDivergence {
  std::int64_t seq = kReplayInitialSeq;
  std::string label;
  ReplayDivergenceKind kind = ReplayDivergenceKind::Value;
  bool has_expected = false;
  bool has_actual = false;
  std::string expected;
  std::string actual;
  std::string preview;

  const char* kind_name() const { return replay_divergence_kind_name(kind); }

  std::string describe() const {
    const std::string where =
        seq == kReplayInitialSeq ? "initial state" : "event seq=" + std::to_string(seq);
    if (kind == ReplayDivergenceKind::Missing)
      return where + ": cell '" + label + "' was not observed on replay";
    if (kind == ReplayDivergenceKind::Unexpected)
      return where + ": cell '" + label + "' appeared on replay but is not in the fingerprint";
    return where + ": cell '" + label + "' expected " + replay_hex(expected) + " but replayed " +
           replay_hex(actual) + (preview.empty() ? "" : ", observed " + preview);
  }
};

// A replayed graph observed a different value than the fingerprint.
class ReplayDivergenceError : public ReplayProofError {
public:
  explicit ReplayDivergenceError(std::vector<ReplayDivergence> divergences)
      : ReplayProofError(build_message(divergences)), divergences_(std::move(divergences)) {}

  const std::vector<ReplayDivergence>& divergences() const { return divergences_; }

  // The earliest divergence, which is the one worth reading.
  const ReplayDivergence& first() const { return divergences_.front(); }

private:
  static std::string build_message(const std::vector<ReplayDivergence>& divergences) {
    if (divergences.empty()) return "replay diverged from the fingerprint";
    std::string message = "replay diverged from the fingerprint: " + divergences.front().describe();
    if (divergences.size() > 1)
      message += " (+" + std::to_string(divergences.size() - 1) + " more)";
    return message;
  }

  std::vector<ReplayDivergence> divergences_;
};

// -- the graph under proof ----------------------------------------------------

// What the harness needs from the graph it rebuilds.
//
// `apply` advances the graph by exactly one event; `observe` returns the cell
// values the fingerprint covers, keyed by a stable label.
class ReplayGraph {
public:
  virtual ~ReplayGraph() = default;
  virtual void apply(const ReplayEvent& event) = 0;
  virtual ReplayObservation observe() const = 0;
};

// -- the harness --------------------------------------------------------------

// Rebuild a graph from an event log and prove it replays identically.
//
// `build` is called once per replay and must return a FRESH graph — a harness
// that reuses one instance proves nothing, since the state it would compare
// against is the state it already has. Returning a `unique_ptr` is what makes
// that structural: there is no instance to hand back twice.
//
// `stride` checkpoints every `stride`-th event; the initial state and the final
// state are always checkpointed. It is recorded in the fingerprint, so a
// fingerprint cannot be compared against a replay that sampled differently.
class ReplayHarness {
public:
  using Builder = std::function<std::unique_ptr<ReplayGraph>()>;

  explicit ReplayHarness(Builder build, int stride = 1)
      : build_(std::move(build)), stride_(stride) {
    if (stride_ < 1)
      throw std::invalid_argument("stride must be >= 1, got " + std::to_string(stride_));
    if (!build_) throw std::invalid_argument("ReplayHarness needs a graph builder");
  }

  int stride() const { return stride_; }

  // Replay `log` once and record what the graph observed.
  ReplayFingerprint record(const ReplayLog& log) const {
    std::vector<ReplayObservation> observed;
    return replay(log, observed);
  }

  // Replay `log` and return the divergences from `fingerprint`.
  //
  // Non-raising for value divergence, so a caller can report all of them. Still
  // throws `ReplayLogMismatchError` for a fingerprint recorded against a
  // different log and `ReplayStrideMismatchError` for one recorded at a
  // different stride: comparing either would answer a question nobody asked.
  std::vector<ReplayDivergence> check(const ReplayLog& log,
                                      const ReplayFingerprint& fingerprint) const {
    std::vector<ReplayObservation> observed;
    const ReplayFingerprint replayed = replay(log, observed);
    revalidate(fingerprint, replayed);
    return compare(fingerprint, replayed, observed);
  }

  // Replay `log` and throw unless it matches `fingerprint` exactly.
  //
  // Returns the freshly recorded fingerprint, which equals `fingerprint`.
  ReplayFingerprint verify(const ReplayLog& log, const ReplayFingerprint& fingerprint) const {
    std::vector<ReplayObservation> observed;
    const ReplayFingerprint replayed = replay(log, observed);
    revalidate(fingerprint, replayed);
    std::vector<ReplayDivergence> divergences = compare(fingerprint, replayed, observed);
    if (!divergences.empty()) throw ReplayDivergenceError(std::move(divergences));
    return replayed;
  }

  // Record `log` and re-replay it, throwing on any divergence.
  //
  // The self-check: no external fingerprint is needed to catch a graph that is
  // not a pure function of its log, because two replays of the same log in the
  // same process already disagree.
  ReplayFingerprint prove(const ReplayLog& log, int replays = 2) const {
    if (replays < 2)
      throw std::invalid_argument("prove needs at least 2 replays to compare, got " +
                                  std::to_string(replays));
    const ReplayFingerprint fingerprint = record(log);
    for (int i = 1; i < replays; ++i)
      verify(log, fingerprint);
    return fingerprint;
  }

private:
  // Bind the fingerprint to these exact log bytes BEFORE comparing any value.
  static void revalidate(const ReplayFingerprint& fingerprint, const ReplayFingerprint& replayed) {
    if (fingerprint.log_digest() != replayed.log_digest())
      throw ReplayLogMismatchError(
          fingerprint.log_digest(), replayed.log_digest(),
          "fingerprint was recorded against a different event log (fingerprint log_digest=" +
              replay_hex(fingerprint.log_digest()) + ", replayed log digest=" +
              replay_hex(replayed.log_digest()) + "); re-record the fingerprint against this log");
    if (fingerprint.stride() != replayed.stride())
      throw ReplayStrideMismatchError(fingerprint.stride(), replayed.stride(),
                                      "fingerprint was recorded at stride " +
                                          std::to_string(fingerprint.stride()) +
                                          " but this harness samples at stride " +
                                          std::to_string(replayed.stride()) + "; re-record it");
  }

  ReplayFingerprint replay(const ReplayLog& log, std::vector<ReplayObservation>& observed) const {
    std::unique_ptr<ReplayGraph> graph = build_();
    if (!graph) throw ReplayProofError("the graph builder returned no graph to replay");
    std::vector<ReplayCheckpoint> checkpoints;
    ReplayObservation sample = graph->observe();
    checkpoints.push_back(replay_checkpoint_of(kReplayInitialSeq, sample));
    observed.push_back(std::move(sample));
    const std::size_t total = log.size();
    for (std::size_t index = 0; index < total; ++index) {
      const ReplayEvent& event = log.events()[index];
      graph->apply(event);
      const std::size_t applied = index + 1;
      if (applied % static_cast<std::size_t>(stride_) == 0 || applied == total) {
        ReplayObservation step = graph->observe();
        checkpoints.push_back(replay_checkpoint_of(event.seq, step));
        observed.push_back(std::move(step));
      }
    }
    return ReplayFingerprint(log.digest(), stride_, std::move(checkpoints));
  }

  static std::vector<ReplayDivergence> compare(const ReplayFingerprint& expected,
                                               const ReplayFingerprint& actual,
                                               const std::vector<ReplayObservation>& observed) {
    std::vector<ReplayDivergence> divergences;
    const std::size_t paired = std::min(expected.checkpoints().size(), actual.checkpoints().size());
    for (std::size_t index = 0; index < paired; ++index) {
      const ReplayCheckpoint& want = expected.checkpoints()[index];
      const ReplayCheckpoint& got = actual.checkpoints()[index];
      const std::map<std::string, std::string> want_cells = want.as_map();
      const std::map<std::string, std::string> got_cells = got.as_map();
      std::vector<std::string> labels;
      for (const auto& cell : want_cells)
        labels.push_back(cell.first);
      for (const auto& cell : got_cells)
        labels.push_back(cell.first);
      std::sort(labels.begin(), labels.end());
      labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
      for (const auto& label : labels) {
        const auto want_it = want_cells.find(label);
        const auto got_it = got_cells.find(label);
        const bool has_want = want_it != want_cells.end();
        const bool has_got = got_it != got_cells.end();
        if (has_want && has_got && want_it->second == got_it->second) continue;
        ReplayDivergence divergence;
        divergence.seq = want.seq;
        divergence.label = label;
        divergence.has_expected = has_want;
        divergence.has_actual = has_got;
        if (has_want) divergence.expected = want_it->second;
        if (has_got) divergence.actual = got_it->second;
        divergence.kind = !has_got    ? ReplayDivergenceKind::Missing
                          : !has_want ? ReplayDivergenceKind::Unexpected
                                      : ReplayDivergenceKind::Value;
        if (index < observed.size()) {
          const auto sampled = observed[index].find(label);
          if (sampled != observed[index].end())
            divergence.preview = replay_preview(sampled->second);
        }
        divergences.push_back(std::move(divergence));
      }
      // The first diverging checkpoint is the actionable one; later ones are
      // almost always the same defect carried forward.
      if (!divergences.empty()) break;
    }
    if (divergences.empty() && expected.checkpoints().size() != actual.checkpoints().size())
      // Same log digest and stride, so this cannot come from sampling — it
      // means `observe` or `apply` changed the checkpoint count.
      throw ReplayProofError("fingerprint has " + std::to_string(expected.checkpoints().size()) +
                             " checkpoints but the replay produced " +
                             std::to_string(actual.checkpoints().size()) + " for the same log");
    return divergences;
  }

  Builder build_;
  int stride_;
};

} // namespace lazily

#endif // LAZILY_REPLAY_HPP
