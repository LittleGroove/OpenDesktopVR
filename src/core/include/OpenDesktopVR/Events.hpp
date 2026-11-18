// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Robert McGough
//
// Events with RAII subscriptions (TH-3, TH-4, doc 02 rule 2).
//
//   Event<int, std::string> changed;
//   Subscription sub = changed.Subscribe([](int id, const std::string& name) { ... });
//   changed.Emit(3, "panel");
//   sub.Reset();   // or let it go out of scope: the listener is really removed
//
// Guarantees:
// - Destroying or resetting a Subscription removes the listener from the
//   table (not just marks it); ListenerCount() drops at once.
// - Emit takes one snapshot under the lock and calls handlers outside it, so
//   handlers may subscribe, unsubscribe or emit without deadlocking.
// - Removed handlers are destroyed outside the lock.
// - A handler removed while an Emit on another thread is in flight is not
//   called after that Emit's snapshot reaches it, unless it was already
//   running.
// - A handler that throws is logged; the other handlers still run.
// - The Event may be destroyed before its subscriptions; they become inert.
#pragma once

#include <OpenDesktopVR/ThreadUtil.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace OpenDesktopVR::Events {

namespace Detail {
class Unsubscriber {
 public:
  virtual ~Unsubscriber() = default;
  virtual void Unsubscribe(std::uint64_t id) noexcept = 0;
};
}// namespace Detail

// Owns one listener registration. Move-only; removes the listener when
// destroyed or reset.
class [[nodiscard]] Subscription {
 public:
  Subscription() = default;
  Subscription(std::weak_ptr<Detail::Unsubscriber> owner, std::uint64_t id) noexcept
    : mOwner(std::move(owner)), mID(id) {
  }
  Subscription(Subscription&& other) noexcept
    : mOwner(std::exchange(other.mOwner, {})), mID(std::exchange(other.mID, 0)) {
  }
  Subscription& operator=(Subscription&& other) noexcept {
    if (this != &other) {
      Reset();
      mOwner = std::exchange(other.mOwner, {});
      mID = std::exchange(other.mID, 0);
    }
    return *this;
  }
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  ~Subscription() {
    Reset();
  }

  void Reset() noexcept {
    if (auto owner = mOwner.lock()) {
      owner->Unsubscribe(mID);
    }
    mOwner.reset();
    mID = 0;
  }

  explicit operator bool() const noexcept {
    return mID != 0 && !mOwner.expired();
  }

 private:
  std::weak_ptr<Detail::Unsubscriber> mOwner;
  std::uint64_t mID {0};
};

// Holds many subscriptions for one owner (a panel, a page): all removed
// together when the owner goes away.
class SubscriptionSet {
 public:
  void Add(Subscription subscription) {
    mSubscriptions.push_back(std::move(subscription));
  }
  SubscriptionSet& operator+=(Subscription subscription) {
    Add(std::move(subscription));
    return *this;
  }
  void Clear() noexcept {
    mSubscriptions.clear();
  }
  std::size_t Size() const noexcept {
    return mSubscriptions.size();
  }

 private:
  std::vector<Subscription> mSubscriptions;
};

template <class... Args>
class Event {
 public:
  using Handler = std::function<void(const Args&...)>;

  Event() : mTable(std::make_shared<Table>()) {
  }
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;

  Subscription Subscribe(Handler handler) {
    return mTable->Add(std::move(handler));
  }

  // Calls every listener registered when the emit starts
  void Emit(const Args&... args) const {
    for (const auto& entry: mTable->Snapshot()) {
      if (!entry->mActive.load(std::memory_order_acquire)) {
        continue;
      }
      ThreadUtil::CatchAll("event handler", [&] { entry->mHandler(args...); });
    }
  }

  std::size_t ListenerCount() const {
    return mTable->Count();
  }

 private:
  struct Entry {
    explicit Entry(Handler handler) : mHandler(std::move(handler)) {
    }
    Handler mHandler;
    std::atomic<bool> mActive {true};
  };
  using EntryPtr = std::shared_ptr<Entry>;

  class Table final : public Detail::Unsubscriber, public std::enable_shared_from_this<Table> {
   public:
    Subscription Add(Handler handler) {
      auto entry = std::make_shared<Entry>(std::move(handler));
      std::uint64_t id = 0;
      {
        std::scoped_lock lock(mMutex);
        id = ++mLastID;
        mEntries.emplace_back(id, std::move(entry));
      }
      return Subscription {this->weak_from_this(), id};
    }

    void Unsubscribe(std::uint64_t id) noexcept override {
      EntryPtr removed;// destroyed after the lock is released (TH-4)
      {
        std::scoped_lock lock(mMutex);
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it) {
          if (it->first == id) {
            // Inactive before it leaves the table, so in-flight snapshots skip it
            it->second->mActive.store(false, std::memory_order_release);
            removed = std::move(it->second);
            mEntries.erase(it);
            break;
          }
        }
      }
    }

    std::vector<EntryPtr> Snapshot() const {
      std::vector<EntryPtr> snapshot;
      std::scoped_lock lock(mMutex);
      snapshot.reserve(mEntries.size());
      for (const auto& [id, entry]: mEntries) {
        snapshot.push_back(entry);
      }
      return snapshot;
    }

    std::size_t Count() const {
      std::scoped_lock lock(mMutex);
      return mEntries.size();
    }

   private:
    mutable ThreadUtil::CheckedMutex mMutex;
    std::vector<std::pair<std::uint64_t, EntryPtr>> mEntries;
    std::uint64_t mLastID {0};
  };

  std::shared_ptr<Table> mTable;
};

}// namespace OpenDesktopVR::Events
