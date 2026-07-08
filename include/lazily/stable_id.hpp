#ifndef LAZILY_STABLE_ID_HPP
#define LAZILY_STABLE_ID_HPP

#include <lazily/types.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lazily {

inline constexpr double kEditThreshold = 0.5;
inline constexpr const char* kAnchorPrefix = "a:";
inline constexpr const char* kContentPrefix = "c:";

inline uint64_t fnv1a_64(const std::string& s) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

inline std::string normalize(const std::string& text) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && isspace((unsigned char)text[start])) start++;
    if (start >= text.size()) break;
    size_t end = start;
    while (end < text.size() && !isspace((unsigned char)text[end])) end++;
    words.push_back(text.substr(start, end - start));
    start = end;
  }
  std::string result;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) result += " ";
    result += words[i];
  }
  return result;
}

inline uint64_t content_hash(const std::string& text) {
  return fnv1a_64(normalize(text));
}

struct Block {
  std::string text;
  std::optional<std::string> anchor;
};

inline Block new_block(std::string text) { return {std::move(text), std::nullopt}; }
inline Block new_anchored_block(std::string anchor, std::string text) {
  return {std::move(text), std::move(anchor)};
}

struct BlockKey {
  enum class Kind { Anchored, Content } kind;
  std::string anchor_value;
  uint64_t content_value;

  bool is_anchored() const { return kind == Kind::Anchored; }
  bool is_content() const { return kind == Kind::Content; }

  bool equals(const BlockKey& o) const {
    if (kind != o.kind) return false;
    if (kind == Kind::Anchored) return anchor_value == o.anchor_value;
    return content_value == o.content_value;
  }

  std::string as_string() const {
    if (kind == Kind::Anchored) {
      return std::string(kAnchorPrefix) + anchor_value;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%016lx", content_value);
    return std::string(kContentPrefix) + buf;
  }
};

inline BlockKey anchored_block_key(std::string value) {
  return {BlockKey::Kind::Anchored, std::move(value), 0};
}
inline BlockKey content_block_key(uint64_t value) {
  return {BlockKey::Kind::Content, "", value};
}
inline BlockKey block_key_of(const Block& block) {
  if (block.anchor) return anchored_block_key(*block.anchor);
  return content_block_key(content_hash(block.text));
}

inline int lcs_len(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.empty() || b.empty()) return 0;
  std::vector<int> prev(b.size() + 1, 0), curr(b.size() + 1, 0);
  for (size_t i = 1; i <= a.size(); ++i) {
    for (size_t j = 1; j <= b.size(); ++j) {
      if (a[i - 1] == b[j - 1])
        curr[j] = prev[j - 1] + 1;
      else
        curr[j] = std::max(prev[j], curr[j - 1]);
    }
    std::swap(prev, curr);
    std::fill(curr.begin(), curr.end(), 0);
  }
  return prev[b.size()];
}

inline std::vector<std::string> split_words(const std::string& text) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && isspace((unsigned char)text[start])) start++;
    if (start >= text.size()) break;
    size_t end = start;
    while (end < text.size() && !isspace((unsigned char)text[end])) end++;
    words.push_back(text.substr(start, end - start));
    start = end;
  }
  return words;
}

inline double similarity(const std::string& a, const std::string& b) {
  auto wa = split_words(a);
  auto wb = split_words(b);
  if (wa.empty() && wb.empty()) return 1.0;
  if (wa.empty() || wb.empty()) return 0.0;
  int lcs = lcs_len(wa, wb);
  return 2.0 * lcs / (wa.size() + wb.size());
}

struct Match {
  enum class Kind { Same, Edited, Inserted };
  Kind kind;
  int old_index;
  double sim;
};

struct Alignment {
  std::vector<Match> new_matches;
  std::vector<int> removed;
};

inline Alignment align(const std::vector<Block>& old_blocks,
                        const std::vector<Block>& new_blocks) {
  Alignment result;

  // Build keys for old blocks
  std::vector<BlockKey> old_keys;
  for (auto& b : old_blocks) old_keys.push_back(block_key_of(b));

  // Track which old indices are used
  std::vector<bool> used(old_blocks.size(), false);

  // Pass 1: exact key match
  for (size_t ni = 0; ni < new_blocks.size(); ++ni) {
    auto nk = block_key_of(new_blocks[ni]);
    int matched = -1;
    for (size_t oi = 0; oi < old_blocks.size(); ++oi) {
      if (!used[oi] && old_keys[oi].equals(nk)) {
        matched = static_cast<int>(oi);
        break;
      }
    }
    if (matched >= 0) {
      used[matched] = true;
      result.new_matches.push_back({Match::Kind::Same, matched, 1.0});
    } else {
      result.new_matches.push_back({Match::Kind::Inserted, -1, 0.0});
    }
  }

  // Pass 2: similarity match for unmatched
  for (size_t ni = 0; ni < new_blocks.size(); ++ni) {
    if (result.new_matches[ni].kind != Match::Kind::Inserted) continue;
    int best_oi = -1;
    double best_sim = 0;
    for (size_t oi = 0; oi < old_blocks.size(); ++oi) {
      if (used[oi]) continue;
      double sim = similarity(old_blocks[oi].text, new_blocks[ni].text);
      if (sim >= kEditThreshold && sim > best_sim) {
        best_sim = sim;
        best_oi = static_cast<int>(oi);
      }
    }
    if (best_oi >= 0) {
      used[best_oi] = true;
      result.new_matches[ni] = {Match::Kind::Edited, best_oi, best_sim};
    }
  }

  // Removed indices
  for (size_t oi = 0; oi < old_blocks.size(); ++oi) {
    if (!used[oi]) result.removed.push_back(static_cast<int>(oi));
  }

  return result;
}

inline std::vector<std::string> assign_stable_keys(
    const std::vector<Block>& old_blocks,
    const std::vector<Block>& new_blocks) {
  auto alignment = align(old_blocks, new_blocks);
  std::vector<std::string> old_keys;
  for (auto& b : old_blocks) old_keys.push_back(block_key_of(b).as_string());

  std::vector<std::string> result;
  for (size_t ni = 0; ni < new_blocks.size(); ++ni) {
    auto& m = alignment.new_matches[ni];
    if (m.kind == Match::Kind::Inserted) {
      result.push_back(block_key_of(new_blocks[ni]).as_string());
    } else {
      result.push_back(old_keys[m.old_index]);
    }
  }
  return result;
}

}  // namespace lazily

#endif  // LAZILY_STABLE_ID_HPP
