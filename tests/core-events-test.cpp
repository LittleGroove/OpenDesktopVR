// SPDX-License-Identifier: LicenseRef-Proprietary
// Copyright (c) 2026 Robert McGough. All rights reserved.
//
// Events: RAII subscriptions really remove listeners (TH-3), the table is
// thread-safe with one snapshot per emit (TH-4), and listener counts stay
// flat across 1,000 settings-style changes (doc 11 "Listener growth").

#include "check.hpp"

#include <OpenDesktopVR/Events.hpp>

#include <atomic>
#include <format>
#include <optional>
#include <thread>
#include <vector>

using namespace OpenDesktopVR::Events;
using namespace OpenDesktopVR::Testing;

namespace {

// A panel that re-subscribes whenever its settings change, the way the
// prototype's listener leak happened
class FakePanel {
 public:
  explicit FakePanel(Event<int>& settingsChanged) : mEvent(settingsChanged) {
    Resubscribe();
  }
  void Resubscribe() {
    // Assigning releases the previous subscription first
    mSubscription = mEvent.Subscribe([this](int) { ++mNotifications; });
  }
  int mNotifications {0};

 private:
  Event<int>& mEvent;
  Subscription mSubscription;
};

}// namespace

int main() {
  Scenario("A subscription receives events until it is destroyed (TH-3)", [] {
    Event<int> event;
    int received = 0;
    {
      auto sub = event.Subscribe([&](int value) { received += value; });
      Check(event.ListenerCount() == 1, "one listener while subscribed");
      event.Emit(2);
      Check(received == 2, "the handler ran");
    }
    Check(event.ListenerCount() == 0, "the listener is removed from the table, not just marked inactive");
    event.Emit(5);
    Check(received == 2, "no calls after the subscription is gone");
  });

  Scenario("Reset, move and SubscriptionSet", [] {
    Event<> event;
    int calls = 0;
    auto a = event.Subscribe([&] { ++calls; });
    Subscription b = std::move(a);
    Check(!a && b, "moving transfers ownership");
    Check(event.ListenerCount() == 1, "still exactly one listener after the move");
    b.Reset();
    Check(event.ListenerCount() == 0 && !b, "Reset removes it");

    SubscriptionSet set;
    for (int i = 0; i < 5; ++i) {
      set += event.Subscribe([&] { ++calls; });
    }
    event.Emit();
    Check(calls == 5 && event.ListenerCount() == 5, "five listeners in a set, all called");
    set.Clear();
    Check(event.ListenerCount() == 0, "clearing the set removes them all");
  });

  Scenario("Listener count stays flat over 1,000 re-subscriptions (listener growth)", [] {
    Event<int> settingsChanged;
    std::vector<std::unique_ptr<FakePanel>> panels;
    for (int i = 0; i < 3; ++i) {
      panels.push_back(std::make_unique<FakePanel>(settingsChanged));
    }
    for (int change = 0; change < 1000; ++change) {
      for (auto& panel: panels) {
        panel->Resubscribe();
      }
      settingsChanged.Emit(change);
    }
    Check(
      settingsChanged.ListenerCount() == 3,
      std::format("3 listeners after 1,000 changes (got {})", settingsChanged.ListenerCount()));
    Check(panels[0]->mNotifications == 1000, "each panel notified exactly once per change");
    panels.clear();
    Check(settingsChanged.ListenerCount() == 0, "destroying the panels removes their listeners");
  });

  Scenario("Handlers can unsubscribe, subscribe and emit from inside a handler (TH-4)", [] {
    Event<int> event;
    std::optional<Subscription> self;
    std::vector<Subscription> added;
    int selfCalls = 0;
    self = event.Subscribe([&](int depth) {
      ++selfCalls;
      self.reset();// unsubscribe itself mid-emit
      added.push_back(event.Subscribe([](int) {}));
      if (depth == 0) {
        event.Emit(1);// re-entrant emit
      }
    });
    event.Emit(0);
    Check(selfCalls == 1, "the self-removing handler ran once, not again from the nested emit");
    Check(event.ListenerCount() == 1, "the listener it added during the emit is registered");
  });

  Scenario("A handler that throws doesn't stop the others", [] {
    Event<> event;
    int after = 0;
    auto a = event.Subscribe([] { throw std::runtime_error("boom"); });
    auto b = event.Subscribe([&] { ++after; });
    event.Emit();
    Check(after == 1, "the second handler still ran");
  });

  Scenario("The event can die before its subscriptions", [] {
    Subscription sub;
    {
      Event<> event;
      sub = event.Subscribe([] {});
    }
    Check(!sub, "the subscription reports it's inert");
    sub.Reset();
    Check(true, "resetting it is harmless");
  });

  Scenario("Concurrent emits, subscribes and unsubscribes (TH-4)", [] {
    Event<int> event;
    std::atomic<bool> stop {false};
    std::atomic<int> calls {0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
      threads.emplace_back([&] {
        while (!stop.load()) {
          event.Emit(1);
        }
      });
    }
    for (int t = 0; t < 3; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 2000; ++i) {
          auto sub = event.Subscribe([&](int v) { calls += v; });
        }
      });
    }
    for (std::size_t t = 3; t < threads.size(); ++t) {
      threads[t].join();
    }
    stop.store(true);
    for (int t = 0; t < 3; ++t) {
      threads[t].join();
    }
    Check(event.ListenerCount() == 0, "no listeners left after 6,000 concurrent subscribe/unsubscribe pairs");
    std::printf("  (%d handler calls during the run)\n", calls.load());
  });

  return Finish();
}
