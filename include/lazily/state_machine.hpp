#ifndef LAZILY_STATE_MACHINE_HPP
#define LAZILY_STATE_MACHINE_HPP

#include <lazily/cell.hpp>
#include <lazily/context.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace lazily {

template <typename S, typename E>
class StateMachine {
 public:
  using TransitionFn = std::function<std::optional<S>(const S&, const E&)>;

  StateMachine(Context& ctx, S initial, TransitionFn transition)
      : state_(ctx.cell(std::move(initial))),
        transition_(std::make_shared<TransitionFn>(std::move(transition))) {}

  bool send(Context& ctx, const E& event) {
    S current = ctx.get_cell(state_);
    auto next = (*transition_)(current, event);
    if (next) {
      ctx.set_cell(state_, *next);
      return true;
    }
    return false;
  }

  S state(Context& ctx) { return ctx.get_cell(state_); }

  CellHandle<S> state_handle() const { return state_; }

  Effect on_transition(Context& ctx,
                              std::function<void(const S&, const S&)> handler) {
    auto state_handle = state_;
    auto prev = std::make_shared<std::optional<S>>();
    auto h = std::make_shared<std::function<void(const S&, const S&)>>(std::move(handler));
    return ctx.effect_void([state_handle, prev, h](Context& c) {
      S current = c.get_cell(state_handle);
      if (*prev && **prev != current) {
        (*h)(**prev, current);
      }
      *prev = current;
    });
  }

  // An eager `Computed<bool>` that tracks whether the machine is in `target`.
  // (`signal` is now the eager-computed convenience — `#lzcellkernel`.)
  Computed<bool> state_is(Context& ctx, S target) {
    auto state_handle = state_;
    return ctx.signal<bool>([state_handle, target](Context& c) {
      return c.get_cell(state_handle) == target;
    });
  }

 private:
  CellHandle<S> state_;
  std::shared_ptr<TransitionFn> transition_;
};

}  // namespace lazily

#endif  // LAZILY_STATE_MACHINE_HPP
