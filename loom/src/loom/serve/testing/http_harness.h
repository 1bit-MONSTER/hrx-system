// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TESTING_HTTP_HARNESS_H_
#define LOOM_SERVE_TESTING_HTTP_HARNESS_H_

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "loom/serve/transport/http/listener.h"

namespace loom_serve::testing {

// Linux integration-test client with explicit service and socket readiness.
// The fixture owns the server/listener until after Join and all client calls.
// Service work is optional: a fixture may instead drive admitted requests
// itself with TakeRequest. No timer substitutes for either ownership boundary.
class HttpHarness {
 public:
  ~HttpHarness();

  iree_status_t Initialize(loom_serve_http_server_t* server,
                           loom_serve_http_listener_t* listener);

  // Starts one outer service owner, retaining its terminal status until Join.
  // Start follows Initialize and any previous owner has already been joined.
  void Start(std::function<iree_status_t()> run);
  bool joinable() const { return thread_.joinable(); }
  // The caller requests shutdown before joining an active service.
  iree_status_t Join();

  // A transport failure ends this client's observation lifetime. Polling
  // callers stop even when an expected-failure test intercepts its diagnostic.
  bool failed() const { return failed_; }

  // Sends one complete request, transferring the connected descriptor to the
  // caller. Read consumes it; tests doing intentional disconnects close it.
  iree_status_t Send(const std::string& request, int* out_client);
  // Reads a close-delimited response and closes the descriptor on every path.
  // Cancellation tests may explicitly permit an empty orderly response.
  iree_status_t Read(int client, bool allow_empty, std::string* out_response);

  // Claims an HTTP request while observing the corresponding client's socket.
  // Only a manually driven fixture calls this: it temporarily owns the server
  // observer, and no router or other request consumer is running concurrently.
  iree_status_t TakeRequest(int client,
                            loom_serve_http_connection_t** out_connection,
                            const loom_serve_http_request_t** out_request);

 private:
  iree_status_t WaitSocket(int client, short events, int notification = -1);
  iree_status_t Diagnose(iree_status_t status);

  // Borrowed transport and listener, owned by the enclosing fixture.
  loom_serve_http_server_t* server_ = nullptr;
  // Borrowed listener supplies the bound ephemeral address.
  loom_serve_http_listener_t* listener_ = nullptr;
  // Readable until Join once the service owner has returned.
  int completion_ = -1;
  // Coalesced request/diagnostic wake while TakeRequest owns the observer.
  int notification_ = -1;
  // Publishes terminal status before the completion descriptor is signaled.
  std::atomic<bool> completed_{false};
  // Written only by the owner; borrowed after acquire and consumed by Join.
  iree_status_t status_ = iree_ok_status();
  // Optional service owner, joined before any fixture dependency is destroyed.
  std::thread thread_;
  // Client-thread-owned terminal transport observation, independent of GTest.
  bool failed_ = false;
};

}  // namespace loom_serve::testing

#endif  // LOOM_SERVE_TESTING_HTTP_HARNESS_H_
