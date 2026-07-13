#ifndef LAZILY_CRDT_TREE_HPP
#define LAZILY_CRDT_TREE_HPP

#include <lazily/crdt.hpp>

#include <type_traits>
#include <utility>

namespace lazily {

// C++17 structural form of the lossless document CRDT contract
// (`#lzcrdttree`). A conforming tree publishes Value/VersionVector/Delta and
// supports identity-preserving frontier snapshots, idempotent delta folds, a
// materialized value, and document join.
template <typename Tree, typename = void> struct CrdtTree : std::false_type {};

template <typename Tree>
struct CrdtTree<
    Tree,
    std::void_t<typename Tree::Value, typename Tree::VersionVector,
                typename Tree::Delta,
                decltype(std::declval<const Tree &>().version_vector()),
                decltype(std::declval<const Tree &>().delta_since(
                    std::declval<const typename Tree::VersionVector &>())),
                decltype(std::declval<Tree &>().apply_delta(
                    std::declval<const typename Tree::Delta &>())),
                decltype(std::declval<const Tree &>().value()),
                decltype(std::declval<Tree &>().merge_from(
                    std::declval<const Tree &>()))>>
    : std::integral_constant<
          bool,
          std::is_same<decltype(std::declval<const Tree &>().version_vector()),
                       typename Tree::VersionVector>::value &&
              std::is_same<
                  decltype(std::declval<const Tree &>().delta_since(
                      std::declval<const typename Tree::VersionVector &>())),
                  typename Tree::Delta>::value &&
              std::is_same<decltype(std::declval<Tree &>().apply_delta(
                               std::declval<const typename Tree::Delta &>())),
                           bool>::value &&
              std::is_same<decltype(std::declval<const Tree &>().value()),
                           typename Tree::Value>::value &&
              std::is_same<decltype(std::declval<Tree &>().merge_from(
                               std::declval<const Tree &>())),
                           bool>::value> {};

template <typename Tree>
inline constexpr bool is_crdt_tree_v = CrdtTree<Tree>::value;

static_assert(is_crdt_tree_v<TextCrdt>, "TextCrdt must satisfy CrdtTree");

} // namespace lazily

#endif // LAZILY_CRDT_TREE_HPP
