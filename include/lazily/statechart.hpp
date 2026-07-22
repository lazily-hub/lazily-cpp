#ifndef LAZILY_STATECHART_HPP
#define LAZILY_STATECHART_HPP

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lazily {

enum class HistoryKind { Shallow, Deep };

enum class Kind { Atomic, Compound, Parallel, Final, HistoryShallow, HistoryDeep };

inline bool is_leaf_kind(Kind k) {
  return k == Kind::Atomic || k == Kind::Final;
}

inline bool is_history_kind(Kind k) {
  return k == Kind::HistoryShallow || k == Kind::HistoryDeep;
}

struct Transition {
  std::string target;
  std::optional<std::string> guard;
  std::vector<std::string> action;
  bool internal = false;
};

struct StateDef {
  std::optional<std::string> parent;
  Kind kind = Kind::Atomic;
  std::optional<std::string> initial;
  std::optional<std::string> default_child;
  std::unordered_map<std::string, Transition> transitions;
  std::vector<std::string> entry;
  std::vector<std::string> exit;
};

using Config = std::set<std::string>;

struct ChartDef {
  std::unordered_map<std::string, StateDef> states;
  std::unordered_map<std::string, std::vector<std::string>> children;
  std::unordered_map<std::string, size_t> order;
  std::unordered_map<std::string, size_t> depth;
  std::string root;

  Kind kind(const std::string& id) const {
    auto it = states.find(id);
    return it != states.end() ? it->second.kind : Kind::Atomic;
  }

  std::vector<std::string> ancestors_inclusive(const std::string& id) const {
    std::vector<std::string> out;
    std::string cur = id;
    while (true) {
      out.push_back(cur);
      auto it = states.find(cur);
      if (it == states.end() || !it->second.parent) break;
      cur = *it->second.parent;
    }
    return out;
  }

  std::string lca(const std::string& a, const std::string& b) const {
    std::set<std::string> anc_a;
    for (auto& s : ancestors_inclusive(a)) anc_a.insert(s);
    for (auto& s : ancestors_inclusive(b)) {
      if (anc_a.count(s)) return s;
    }
    return root;
  }

  bool is_proper_descendant(const std::string& desc,
                             const std::string& anc) const {
    if (desc == anc) return false;
    for (auto& s : ancestors_inclusive(desc)) {
      if (s == anc) return true;
    }
    return false;
  }

  size_t get_depth(const std::string& id) const {
    auto it = depth.find(id);
    return it != depth.end() ? it->second : 0;
  }
};

using Recording = std::variant<std::string, std::set<std::string>>;
using HistoryMap = std::unordered_map<std::string, Recording>;

class TransitionBuilder {
 public:
  static TransitionBuilder to(std::string target) {
    return TransitionBuilder{Transition{std::move(target), std::nullopt, {}, false}};
  }
  TransitionBuilder& guard(std::string name) {
    inner_.guard = std::move(name);
    return *this;
  }
  TransitionBuilder& action(std::string name) {
    inner_.action.push_back(std::move(name));
    return *this;
  }
  TransitionBuilder& internal() {
    inner_.internal = true;
    return *this;
  }
  Transition build() { return std::move(inner_); }

 private:
  explicit TransitionBuilder(Transition t) : inner_(std::move(t)) {}
  Transition inner_;
  friend class StateBuilder;
};

class StateBuilder {
 public:
  static StateBuilder atomic(std::string id) { return with_kind(std::move(id), Kind::Atomic); }
  static StateBuilder compound(std::string id, std::string initial) {
    StateBuilder sb(std::move(id), Kind::Compound);
    sb.def_.initial = std::move(initial);
    return sb;
  }
  static StateBuilder parallel(std::string id) { return with_kind(std::move(id), Kind::Parallel); }
  static StateBuilder final_state(std::string id) { return with_kind(std::move(id), Kind::Final); }
  static StateBuilder history_shallow(std::string id) {
    return with_kind(std::move(id), Kind::HistoryShallow);
  }
  static StateBuilder history_deep(std::string id) {
    return with_kind(std::move(id), Kind::HistoryDeep);
  }

  StateBuilder& parent(std::string p) { def_.parent = std::move(p); return *this; }
  StateBuilder& default_child(std::string target) { def_.default_child = std::move(target); return *this; }
  StateBuilder& entry(std::string action) { def_.entry.push_back(std::move(action)); return *this; }
  StateBuilder& exit(std::string action) { def_.exit.push_back(std::move(action)); return *this; }
  StateBuilder& on(std::string event, std::string target) {
    def_.transitions[event] = TransitionBuilder::to(std::move(target)).build();
    return *this;
  }
  StateBuilder& on_guarded(std::string event, std::string target, std::string guard) {
    def_.transitions[event] = TransitionBuilder::to(std::move(target)).guard(std::move(guard)).build();
    return *this;
  }
  StateBuilder& on_transition(std::string event, TransitionBuilder t) {
    def_.transitions[event] = std::move(t).build();
    return *this;
  }

  std::string id;
  StateDef def_;

 private:
  static StateBuilder with_kind(std::string id, Kind kind) {
    return StateBuilder(std::move(id), kind);
  }
  StateBuilder(std::string id, Kind kind) : id(std::move(id)) {
    def_.kind = kind;
  }
};

class ChartBuilder {
 public:
  ChartBuilder& state(StateBuilder sb) {
    states_.push_back(std::move(sb));
    return *this;
  }

  std::optional<ChartDef> build() {
    std::unordered_map<std::string, StateDef> states;
    std::unordered_map<std::string, size_t> order;
    for (size_t i = 0; i < states_.size(); ++i) {
      if (states.count(states_[i].id)) return std::nullopt;
      order[states_[i].id] = i;
      states[states_[i].id] = std::move(states_[i].def_);
    }
    return from_states(std::move(states), std::move(order));
  }

 private:
  std::vector<StateBuilder> states_;

  static std::optional<ChartDef> from_states(
      std::unordered_map<std::string, StateDef> states,
      std::unordered_map<std::string, size_t> order) {
    std::unordered_map<std::string, std::vector<std::string>> children;
    std::optional<std::string> root;
    for (auto& [id, def] : states) {
      if (def.parent) {
        children[*def.parent].push_back(id);
      } else {
        if (root) return std::nullopt;
        root = id;
      }
    }
    if (!root) return std::nullopt;
    for (auto& [_, kids] : children) {
      std::sort(kids.begin(), kids.end(),
                [&](const std::string& a, const std::string& b) {
                  auto ia = order.find(a);
                  auto ib = order.find(b);
                  size_t va = ia != order.end() ? ia->second : SIZE_MAX;
                  size_t vb = ib != order.end() ? ib->second : SIZE_MAX;
                  return va < vb;
                });
    }
    std::unordered_map<std::string, size_t> depth;
    compute_depth(states, *root, 0, depth);
    return ChartDef{std::move(states), std::move(children), std::move(order),
                     std::move(depth), std::move(*root)};
  }

  static void compute_depth(
      const std::unordered_map<std::string, StateDef>& states,
      const std::string& id, size_t current,
      std::unordered_map<std::string, size_t>& out) {
    out[id] = current;
    for (auto& [cid, def] : states) {
      if (def.parent && *def.parent == id)
        compute_depth(states, cid, current + 1, out);
    }
  }
};

// -- Context-free transition engine --

inline bool guard_passes(const Transition& t,
                          const std::unordered_map<std::string, bool>& guards) {
  if (!t.guard) return true;
  auto it = guards.find(*t.guard);
  return it != guards.end() ? it->second : false;
}

inline void enter_subtree(const ChartDef& def, const std::string& state,
                           Config& enter, std::vector<std::string>& actions) {
  enter.insert(state);
  auto it = def.states.find(state);
  if (it != def.states.end()) {
    for (auto& a : it->second.entry) actions.push_back(a);
  }
  Kind k = def.kind(state);
  if (is_leaf_kind(k) || is_history_kind(k)) return;
  if (k == Kind::Compound) {
    if (it != def.states.end() && it->second.initial) {
      enter_subtree(def, *it->second.initial, enter, actions);
    }
  } else if (k == Kind::Parallel) {
    auto cit = def.children.find(state);
    if (cit != def.children.end()) {
      for (auto& region : cit->second)
        enter_subtree(def, region, enter, actions);
    }
  }
}

inline std::vector<std::string> path_below(const ChartDef& def,
                                             const std::string& lca,
                                             const std::string& target) {
  auto chain = def.ancestors_inclusive(target);
  size_t idx = chain.size();
  for (size_t i = 0; i < chain.size(); ++i) {
    if (chain[i] == lca) { idx = i; break; }
  }
  chain.resize(idx);
  std::reverse(chain.begin(), chain.end());
  return chain;
}

inline std::optional<std::string> history_child_of(const ChartDef& def,
                                                     const std::string& region) {
  auto it = def.children.find(region);
  if (it == def.children.end()) return std::nullopt;
  for (auto& k : it->second) {
    if (is_history_kind(def.kind(k))) return k;
  }
  return std::nullopt;
}

inline void record_region(const ChartDef& def, const std::string& region,
                           const std::string& hist_child, const Config& config,
                           HistoryMap& history) {
  Kind k = def.kind(hist_child);
  if (k == Kind::HistoryShallow) {
    auto it = def.children.find(region);
    if (it == def.children.end()) return;
    for (auto& c : it->second) {
      if (config.count(c) && !is_history_kind(def.kind(c))) {
        history[hist_child] = c;
        return;
      }
    }
  } else if (k == Kind::HistoryDeep) {
    std::set<std::string> set;
    for (auto& s : config) {
      if (def.is_proper_descendant(s, region)) set.insert(s);
    }
    history[hist_child] = std::move(set);
  }
}

inline void restore_via_history(const ChartDef& def, const HistoryMap& history,
                                 const std::string& hist,
                                 const std::string& region, Config& enter) {
  auto it = history.find(hist);
  if (it == history.end()) {
    auto hit = def.states.find(hist);
    std::string start;
    if (hit != def.states.end() && hit->second.default_child)
      start = *hit->second.default_child;
    else {
      auto rit = def.states.find(region);
      if (rit != def.states.end() && rit->second.initial) start = *rit->second.initial;
    }
    if (!start.empty()) {
      for (auto& s : path_below(def, region, start)) enter.insert(s);
      std::vector<std::string> tmp;
      enter_subtree(def, start, enter, tmp);
    }
    return;
  }
  if (std::holds_alternative<std::string>(it->second)) {
    std::string child = std::get<std::string>(it->second);
    enter.insert(child);
    std::vector<std::string> tmp;
    enter_subtree(def, child, enter, tmp);
  } else {
    for (auto& s : std::get<std::set<std::string>>(it->second)) enter.insert(s);
  }
}

inline std::pair<Config, Config> compute_exit_enter(
    const ChartDef& def, const HistoryMap& history,
    const std::string& source, const Transition& transition,
    const std::string& leaf, const Config& config) {
  const std::string& target = transition.target;
  bool internal = transition.internal &&
                  (target == source || def.is_proper_descendant(target, source));
  std::string lca = internal ? source : def.lca(leaf, target);

  Config exit_set;
  for (auto& s : config) {
    if (def.is_proper_descendant(s, lca)) exit_set.insert(s);
  }

  Config enter;
  Kind tk = def.kind(target);
  if (is_history_kind(tk)) {
    auto tit = def.states.find(target);
    std::string region = (tit != def.states.end() && tit->second.parent) ? *tit->second.parent : def.root;
    for (auto& s : path_below(def, lca, region)) enter.insert(s);
    restore_via_history(def, history, target, region, enter);
  } else {
    for (auto& s : path_below(def, lca, target)) enter.insert(s);
    std::vector<std::string> tmp;
    enter_subtree(def, target, enter, tmp);
  }
  return {exit_set, enter};
}

inline std::optional<std::pair<Config, std::vector<std::string>>> engine_send(
    const ChartDef& def, HistoryMap& history, const Config& config,
    const std::string& event,
    const std::unordered_map<std::string, bool>& guards) {
  struct Cand {
    std::string source;
    const Transition* transition;
    std::string leaf;
  };
  std::vector<Cand> candidates;
  for (auto& leaf : config) {
    if (!is_leaf_kind(def.kind(leaf))) continue;
    for (auto& anc : def.ancestors_inclusive(leaf)) {
      auto sit = def.states.find(anc);
      if (sit == def.states.end()) continue;
      auto tit = sit->second.transitions.find(event);
      if (tit == sit->second.transitions.end()) continue;
      if (!guard_passes(tit->second, guards)) continue;
      candidates.push_back({anc, &tit->second, leaf});
      break;
    }
  }
  if (candidates.empty()) return std::nullopt;

  std::sort(candidates.begin(), candidates.end(), [&](const Cand& a, const Cand& b) {
    size_t da = def.get_depth(a.source);
    size_t db = def.get_depth(b.source);
    if (da != db) return da > db;
    auto oa = def.order.find(a.source);
    auto ob = def.order.find(b.source);
    size_t va = oa != def.order.end() ? oa->second : SIZE_MAX;
    size_t vb = ob != def.order.end() ? ob->second : SIZE_MAX;
    return va < vb;
  });

  Config exit_union, enter_union;
  std::vector<const Transition*> taken;
  for (auto& cand : candidates) {
    auto [exit_set, enter_set] = compute_exit_enter(
        def, history, cand.source, *cand.transition, cand.leaf, config);
    bool conflict = false;
    for (auto& s : exit_set) {
      if (exit_union.count(s)) { conflict = true; break; }
    }
    if (conflict) continue;
    exit_union.insert(exit_set.begin(), exit_set.end());
    enter_union.insert(enter_set.begin(), enter_set.end());
    taken.push_back(cand.transition);
  }
  if (taken.empty()) return std::nullopt;

  for (auto& s : exit_union) {
    if (auto hc = history_child_of(def, s))
      record_region(def, s, *hc, config, history);
  }

  std::vector<std::string> actions;
  std::vector<std::string> exit_sorted(exit_union.begin(), exit_union.end());
  std::sort(exit_sorted.begin(), exit_sorted.end(),
             [&](const std::string& a, const std::string& b) {
               return def.get_depth(a) > def.get_depth(b);
             });
  for (auto& s : exit_sorted) {
    auto it = def.states.find(s);
    if (it != def.states.end()) for (auto& a : it->second.exit) actions.push_back(a);
  }
  for (auto t : taken) {
    for (auto& a : t->action) actions.push_back(a);
  }
  std::vector<std::string> enter_sorted(enter_union.begin(), enter_union.end());
  std::sort(enter_sorted.begin(), enter_sorted.end(),
            [&](const std::string& a, const std::string& b) {
              return def.get_depth(a) < def.get_depth(b);
            });
  for (auto& s : enter_sorted) {
    auto it = def.states.find(s);
    if (it != def.states.end()) for (auto& a : it->second.entry) actions.push_back(a);
  }

  Config new_config = config;
  for (auto& s : exit_union) new_config.erase(s);
  for (auto& s : enter_union) new_config.insert(s);

  return std::make_pair(std::move(new_config), std::move(actions));
}

// -- Reactive StateChart --

class StateChart {
 public:
  StateChart(Context& ctx, ChartDef def) : def_(std::move(def)) {
    Config enter;
    std::vector<std::string> actions;
    enter_subtree(def_, def_.root, enter, actions);
    config_ = ctx.source(std::move(enter));
    last_actions_ = std::move(actions);
  }

  std::vector<std::string> last_actions() const { return last_actions_; }

  Config configuration(Context& ctx) { return ctx.get(config_); }

  std::vector<std::string> active_leaves(Context& ctx) {
    auto config = configuration(ctx);
    std::vector<std::string> leaves;
    for (auto& id : config) {
      if (is_leaf_kind(def_.kind(id))) leaves.push_back(id);
    }
    std::sort(leaves.begin(), leaves.end());
    return leaves;
  }

  bool matches(Context& ctx, const std::string& id) {
    return configuration(ctx).count(id) > 0;
  }

  bool send(Context& ctx, const std::string& event,
            const std::unordered_map<std::string, bool>& guards) {
    auto config = configuration(ctx);
    auto result = engine_send(def_, history_, config, event, guards);
    if (!result) {
      last_actions_.clear();
      return false;
    }
    auto& [new_config, actions] = *result;
    last_actions_ = std::move(actions);
    if (new_config != config) {
      ctx.set(config_, std::move(new_config));
    }
    return true;
  }

 private:
  ChartDef def_;
  Source<Config> config_;
  HistoryMap history_;
  std::vector<std::string> last_actions_;
};

}  // namespace lazily

#endif  // LAZILY_STATECHART_HPP
