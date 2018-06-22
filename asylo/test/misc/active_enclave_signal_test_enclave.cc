/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <future>

#include "asylo/test/misc/signal_test.pb.h"
#include "asylo/test/util/enclave_test_application.h"
#include "asylo/util/posix_error_space.h"
#include "asylo/util/status.h"

namespace asylo {

constexpr int kTimeout = 10;

static thread_local volatile bool signal_handled = false;
static thread_local volatile bool blocked_signal_handled = false;
static thread_local volatile bool signal_handler_interrupted = false;

void HandleSignalWithHandler(int signum) {
  if (signum == SIGUSR1) {
    signal_handled = true;
  }
  if (signum == SIGUSR2) {
    blocked_signal_handled = true;
  }
}

void HandleSignalWithSigActionMask(int signum) {
  if (signum == SIGUSR1) {
    signal_handled = true;
  }
  if (blocked_signal_handled) {
    signal_handler_interrupted = true;
  }
}

void HandleSignalWithSigAction(int signum, siginfo_t *info, void *ucontext) {
  if (signum == SIGUSR1) {
    signal_handled = true;
  }
}

class ActiveEnclaveSignalTest : public TrustedApplication {
 public:
  ActiveEnclaveSignalTest() = default;

  Status Run(const EnclaveInput &input, EnclaveOutput *output) {
    if (!input.HasExtension(signal_test_input)) {
      return Status(error::GoogleError::INVALID_ARGUMENT,
                    "Missing input extension");
    }
    SignalTestInput test_input = input.GetExtension(signal_test_input);
    if (!test_input.has_signal_test_type()) {
      return Status(error::GoogleError::INVALID_ARGUMENT,
                    "Missing signal_test_type");
    }
    Status status;
    std::future<Status> mask_result;
    switch (test_input.signal_test_type()) {
      case SignalTestInput::HANDLER:
        return RunSignalTest(SignalTestInput::HANDLER);
      case SignalTestInput::SIGNAL:
        return RunSignalTest(SignalTestInput::SIGNAL);
      case SignalTestInput::SIGACTION:
        return RunSignalTest(SignalTestInput::SIGACTION);
      case SignalTestInput::SIGMASK:
        mask_result = std::async(std::launch::async,
                                 &ActiveEnclaveSignalTest::SetSignalMask, this);
        status = RunSignalTest(SignalTestInput::SIGMASK);
        // If |mask_result| fails, that means the call to sigprocmask fails. And
        // |status| will fail to the same reason in that case.
        if (!mask_result.get().ok()) {
          return mask_result.get();
        }
        return status;
      case SignalTestInput::SIGACTIONMASK:
        return RunSignalTest(SignalTestInput::SIGACTIONMASK);
      default:
        return Status(error::GoogleError::INVALID_ARGUMENT,
                      "No vaild test type");
    }
  }

 private:
  Status RunSignalTest(const SignalTestInput::SignalTestType &test_type) {
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (test_type == SignalTestInput::SIGMASK) {
      if (sigprocmask(SIG_BLOCK, &set, &oldset) != 0) {
        return Status(static_cast<error::PosixError>(errno),
                      "Failed to block signal");
      }
    }
    struct sigaction act, oldact;
    switch (test_type) {
      case SignalTestInput::HANDLER:
        act.sa_handler = &HandleSignalWithHandler;
        break;
      case SignalTestInput::SIGNAL:
        signal(SIGUSR1, &HandleSignalWithHandler);
        break;
      case SignalTestInput::SIGACTION:
        act.sa_sigaction = &HandleSignalWithSigAction;
        act.sa_flags |= SA_SIGINFO;
        break;
      case SignalTestInput::SIGMASK:
        act.sa_handler = &HandleSignalWithHandler;
        break;
      case SignalTestInput::SIGACTIONMASK:
        // Register a handler for SIGUSR2.
        act.sa_handler = &HandleSignalWithHandler;
        sigaction(SIGUSR2, &act, &oldact);
        // Block SIGUSR2 during execution of SIGUSR1 handler.
        act.sa_handler = &HandleSignalWithSigActionMask;
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR2);
        act.sa_mask = mask;
        break;
      default:
        return Status(error::GoogleError::INVALID_ARGUMENT,
                      "No valid test type");
    }
    if (test_type != SignalTestInput::SIGNAL) {
      sigaction(SIGUSR1, &act, &oldact);
    }
    // Print to the pipe so that the signal thread will start sending the
    // signal.
    printf("ready to receive signal!");
    fclose(stdout);
    // Wait till the signal is received. Time out in 10 seconds.
    int count = 0;
    bool all_signals_handled = false;
    // For SIGACTIONMASK tests, both SIGUSR1 and SIGUSR2 are sent, both should
    // be handled by the enclave. For all other cases only SIGUSR1 is expected.
    while (!all_signals_handled && ++count < kTimeout) {
      switch (test_type) {
        case SignalTestInput::SIGACTIONMASK:
          all_signals_handled = signal_handled && blocked_signal_handled;
          break;
        default:
          all_signals_handled = signal_handled;
      }
      sleep(1);
    }
    if (test_type == SignalTestInput::SIGACTIONMASK &&
        signal_handler_interrupted) {
      return Status(error::GoogleError::INTERNAL,
                    "Signal handler interrupted by a masked signal");
    }
    // For signal tests other then SIGMASK, the signal should have been handled
    // by now.
    if (test_type != SignalTestInput::SIGMASK) {
      return all_signals_handled
                 ? Status::OkStatus()
                 : Status(error::GoogleError::INTERNAL, "Signal not received");
    }
    // For signal mask test, signal should not have been handled since it's
    // blocked.
    if (signal_handled) {
      return Status(error::GoogleError::INTERNAL,
                    "Signal received when it's blocked");
    }
    if (sigprocmask(SIG_UNBLOCK, &set, &oldset) != 0) {
      return Status(static_cast<error::PosixError>(errno),
                    "Failed to unblock signal");
    }
    // The queued signal should have been handled by now.
    return signal_handled ? Status::OkStatus()
                          : Status(error::GoogleError::INTERNAL,
                                   "Signal not received after unblocked");
  }

  // Keeps unblocking the signal to test whether the signal mask in the other
  // thread is affected.
  Status SetSignalMask() {
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    for (int i = 0; i < kTimeout; ++i) {
      if (sigprocmask(SIG_UNBLOCK, &set, &oldset) != 0) {
        return Status(static_cast<error::PosixError>(errno),
                      "Failed to unblock signal");
      }
      sleep(1);
    }
    return Status::OkStatus();
  }
};

TrustedApplication *BuildTrustedApplication() {
  return new ActiveEnclaveSignalTest;
}

}  // namespace asylo
