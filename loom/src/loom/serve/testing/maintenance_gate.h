// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TESTING_MAINTENANCE_GATE_H_
#define LOOM_SERVE_TESTING_MAINTENANCE_GATE_H_

#include <condition_variable>
#include <mutex>

#include "iree/hal/memory/maintenance.h"

namespace loom_serve::testing {

// Holds the real native cold executor while unrelated work must progress.
// The outer test thread owns this gate; no timeout stands in for readiness.
// Destruction releases and joins the callback, including after failed
// assertions.
class MaintenanceGate {
 public:
  explicit MaintenanceGate(iree_hal_memory_maintenance_t* maintenance) {
    work_.entry.fn = Run;
    work_.owner = this;
    iree_hal_memory_maintenance_enqueue(maintenance, &work_.entry);
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return entered_; });
  }

  ~MaintenanceGate() { Release(); }

  void Release() {
    std::unique_lock<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return retired_; });
  }

 private:
  struct loom_serve_test_maintenance_work_t {
    // Native executor queue entry, first for the callback's enclosing cast.
    iree_hal_memory_maintenance_entry_t entry = {};
    // Borrowed gate, joined by its destructor.
    MaintenanceGate* owner = nullptr;
  };

  static void Run(iree_hal_memory_maintenance_entry_t* entry) {
    auto* self =
        reinterpret_cast<loom_serve_test_maintenance_work_t*>(entry)->owner;
    std::unique_lock<std::mutex> lock(self->mutex_);
    self->entered_ = true;
    self->condition_.notify_all();
    self->condition_.wait(lock, [self] { return self->released_; });
    self->retired_ = true;
    self->condition_.notify_all();
  }

  // Queued work remains alive until native callback retirement.
  loom_serve_test_maintenance_work_t work_;
  // Serializes callback entry, release, and final gate access.
  std::mutex mutex_;
  // Synchronizes only the outer test thread and native cold executor.
  std::condition_variable condition_;
  // The cold executor is inside the controlled hold.
  bool entered_ = false;
  // The outer owner permits native progress.
  bool released_ = false;
  // The callback has finished all access except unlocking mutex_.
  bool retired_ = false;
};

}  // namespace loom_serve::testing

#endif  // LOOM_SERVE_TESTING_MAINTENANCE_GATE_H_
