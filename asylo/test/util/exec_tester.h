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

#ifndef ASYLO_TEST_UTIL_EXEC_TESTER_H_
#define ASYLO_TEST_UTIL_EXEC_TESTER_H_

#include <unistd.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace asylo {
namespace experimental {

/// Executes a subprocess. Monitors its output to a given file descriptor
/// (stdout by default) and checks its exit code.
class ExecTester {
 public:
  /// Constructs an `ExecTester` that will monitor an `execve` call on `args`.
  ///
  /// \param args The command-line arguments to the subprocess. The first
  ///             argument should be the executable to be run.
  /// \param fd_to_check The file descriptor from which output is sent to
  ///                    TestLine().
  ExecTester(const std::vector<std::string> &args, int fd_to_check = STDOUT_FILENO);
  virtual ~ExecTester() = default;

  /// Forks and execs the subprocess with the configured arguments. Redirects
  /// the subprocess's stdin from `input` if non-empty. Validates the
  /// subprocess's output to `fd_to_check` (from the constructor) with
  /// TestLine() and FinalCheck(). Stores the process status in `status` after
  /// exit or signal termination.
  ///
  /// \param input The input to give to the subprocess on its stdin.
  /// \param[out] status An output argument that is set to the subprocess's exit
  ///                    code.
  /// \return The logical "and" of all TestLine() results on the subprocess's
  ///         output to the configured file descriptor.
  bool Run(const std::string &input, int *status);

  /// Returns `file_name` qualified to be in the same directory as the file
  /// specified by `path`. This utility helps find binaries in common use cases
  /// in Asylo.
  ///
  /// \param path A path to a file.
  /// \param file_name A path to a file relative to the directory containing
  ///                  `path`.
  /// \return A path to `file_name` within the same directory as the file at
  ///         `path`. If `path` is a relative path, then the returned path is
  ///         relative to the same directory. If `path` is absolute, then so is
  ///         the returned path.
  static std::string BuildSiblingPath(const std::string &path, const std::string &file_name);

 protected:
  /// Checks a line of the subprocess's output to the configured file descriptor
  /// for an expected property.
  ///
  /// \param line The line to check.
  /// \return `true` if the property holds and `false` otherwise.
  virtual bool CheckLine(const std::string &line) { return true; }

  /// Returns the final result given the accumulated TestLine() results. This is
  /// useful e.g., for determining hard bounds that TestLine() soft-checks.
  ///
  /// \param accumulated The conjunction (logical "and") of the return value of
  ///                    TestLine() on each line of the subprocess's output to
  ///                    the given file descriptor.
  /// \return Whether the test as a whole was successful.
  virtual bool FinalCheck(bool accumulated) { return accumulated; }

 private:
  // ASSERT_* statements expect a void return type.
  void RunWithAsserts(const std::string &input, bool *result, int *status);

  // Redirects subprocess stdin and the configured fd with pipes and executes
  // the subprocess.
  void DoExec(int read_stdin, int write_stdin, int read_fd_to_check,
              int write_fd_to_check);

  // Reads contents of `fd` into `buf` and runs TestLine() on each
  // newline-terminated piece of `buf` as written to `linebuf`. Stores any
  // unfinished line in `linebuf`, i.e., the characters in `buf` that follow the
  // last newline. The accumulated TestLine() results are stored in `result`.
  void CheckFD(int fd, char *buf, size_t bufsize, std::stringstream *linebuf,
               bool *result);

  // Polls output from `pid` to `fd` and calls CheckFD() to accumulate
  // TestLine() results. Sets `status` to `pid`'s termination status.
  void ReadCheckLoop(pid_t pid, int fd, bool *result, int *status);

  std::vector<std::string> args_;
  int fd_to_check_;
};

}  // namespace experimental
}  // namespace asylo

#endif  // ASYLO_TEST_UTIL_EXEC_TESTER_H_
