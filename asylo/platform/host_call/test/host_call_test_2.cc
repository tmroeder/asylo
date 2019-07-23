/*
 *
 * Copyright 2019 Asylo authors
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

#include <fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/flags/flag.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "asylo/enclave_manager.h"
#include "asylo/platform/host_call/test/enclave_test_selectors.h"
#include "asylo/platform/host_call/untrusted/host_call_handlers_initializer.h"
#include "asylo/platform/primitives/extent.h"
#include "asylo/platform/primitives/test/test_backend.h"
#include "asylo/platform/primitives/untrusted_primitives.h"
#include "asylo/platform/primitives/util/message.h"
#include "asylo/platform/storage/utils/fd_closer.h"
#include "asylo/platform/system_call/type_conversions/types_functions.h"
#include "asylo/test/util/status_matchers.h"
#include "asylo/test/util/test_flags.h"

using ::testing::Eq;
using ::testing::Gt;
using ::testing::Not;
using ::testing::SizeIs;
using ::testing::StrEq;

namespace asylo {
namespace host_call {
namespace {

class HostCallTest : public ::testing::Test {
 protected:
  // Loads the enclave. The function uses the factory method
  // |primitives::test::TestBackend::Get()| for loading the enclave, and the
  // type of backend (sim, remote, sgx etc.) loaded depends upon the type of
  // library included with the build that implements the abstract factory class
  // |TestBackend|.
  std::shared_ptr<primitives::Client> LoadTestEnclaveOrDie(
      StatusOr<std::unique_ptr<primitives::Client::ExitCallProvider>>
          exit_call_provider = GetHostCallHandlersMapping()) {
    ASYLO_EXPECT_OK(exit_call_provider);
    const auto client =
        primitives::test::TestBackend::Get()->LoadTestEnclaveOrDie(
            /*enclave_name=*/"host_call_test_enclave",
            std::move(exit_call_provider.ValueOrDie()));

    return client;
  }

  void SetUp() override {
    EnclaveManager::Configure(EnclaveManagerOptions());
    client_ = LoadTestEnclaveOrDie();
    ASSERT_FALSE(client_->IsClosed());
  }

  void TearDown() override {
    client_->Destroy();
    EXPECT_TRUE(client_->IsClosed());
  }

  std::shared_ptr<primitives::Client> client_;
};

// Tests enc_untrusted_truncate() by making a call from inside the enclave and
// verifying that the file is indeed truncated on the untrusted side by reading
// the file.
TEST_F(HostCallTest, TestTruncate) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Write something to the file.
  std::string file_content = "some random content.";
  ASSERT_THAT(write(fd, file_content.c_str(), file_content.length() + 1),
              Eq(file_content.length() + 1));

  primitives::MessageWriter in;
  constexpr int kTruncLen = 5;
  in.Push(test_file);
  in.Push<off_t>(/*value=length=*/kTruncLen);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestTruncate, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), 0);

  // Verify contents of the file by reading it.
  char read_buf[10];
  ASSERT_THAT(lseek(fd, 0, SEEK_SET), Eq(0));
  EXPECT_THAT(read(fd, read_buf, 10), Eq(kTruncLen));
  read_buf[kTruncLen] = '\0';
  EXPECT_THAT(read_buf, StrEq(file_content.substr(0, kTruncLen)));
}

// Tests enc_untrusted_ftruncate() by making a call from inside the enclave and
// verifying that the file is indeed truncated on the untrusted side by reading
// the file.
TEST_F(HostCallTest, TestFTruncate) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Write something to the file.
  std::string file_content = "some random content.";
  ASSERT_THAT(write(fd, file_content.c_str(), file_content.length() + 1),
              Eq(file_content.length() + 1));

  primitives::MessageWriter in2;
  constexpr int kTruncLen = 5;
  in2.Push<int>(/*value=fd=*/fd);
  in2.Push<off_t>(/*value=length=*/kTruncLen);

  primitives::MessageReader out2;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFTruncate, &in2, &out2));
  ASSERT_THAT(out2, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out2.next<int>(), 0);

  // Verify contents of the file by reading it.
  char read_buf[10];
  ASSERT_THAT(lseek(fd, 0, SEEK_SET), Eq(0));
  EXPECT_THAT(read(fd, read_buf, 10), Eq(kTruncLen));
  read_buf[kTruncLen] = '\0';
  EXPECT_THAT(read_buf, StrEq(file_content.substr(0, kTruncLen)));

  // Force an error and verify that the return value is non-zero.
  primitives::MessageWriter in3;
  in3.Push<int>(/*value=fd=*/-1);
  in3.Push<off_t>(/*value=length=*/kTruncLen);

  primitives::MessageReader out3;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFTruncate, &in3, &out3));
  ASSERT_THAT(out3, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out3.next<int>(), -1);
}

// Tests enc_untrusted_rmdir() by making a call from inside the enclave and
// verifying that the directory is indeed deleted.
TEST_F(HostCallTest, TestRmdir) {
  std::string dir_to_del =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/dir_to_del");
  ASSERT_THAT(mkdir(dir_to_del.c_str(), O_CREAT | O_RDWR), Eq(0));

  primitives::MessageWriter in;
  in.Push(dir_to_del);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestRmdir, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));

  // Verify that the directory does not exist.
  struct stat sb;
  EXPECT_FALSE(stat(dir_to_del.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode));
}

// Tests enc_untrusted_socket() by trying to obtain a valid (greater than 0)
// socket file descriptor when the method is called from inside the enclave.
TEST_F(HostCallTest, TestSocket) {
  primitives::MessageWriter in;
  // Setup bidirectional IPv6 socket.
  in.Push<int>(/*value=domain=*/AF_INET6);
  in.Push<int>(/*value=type=*/SOCK_STREAM);
  in.Push<int>(/*value=protocol=*/0);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestSocket, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Gt(0));

  // Setup socket for local bidirectional communication between two processes on
  // the host.
  primitives::MessageWriter in2;
  in2.Push<int>(/*value=domain=*/AF_UNIX);
  in2.Push<int>(/*value=type=*/SOCK_STREAM);
  in2.Push<int>(/*value=protocol=*/0);

  primitives::MessageReader out2;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestSocket, &in2, &out2));
  ASSERT_THAT(out2, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out2.next<int>(), Gt(0));
}

// Tests enc_untrusted_listen() by creating a local socket and calling
// enc_untrusted_listen() on the socket. Checks to make sure that listen returns
// 0, then creates a client socket and attempts to connect to the address of
// the local socket. The connect attempt will only succeed if the listen call
// is successful.
TEST_F(HostCallTest, TestListen) {
  // Create a local socket and ensure that it is valid (fd > 0).
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_THAT(socket_fd, Gt(0));

  std::string sockpath =
      absl::StrCat("/tmp/", absl::ToUnixNanos(absl::Now()), ".sock");

  // Create a local socket address and bind the socket to it.
  sockaddr_un sa = {};
  sa.sun_family = AF_UNIX;
  strncpy(&sa.sun_path[0], sockpath.c_str(), sizeof(sa.sun_path) - 1);
  EXPECT_THAT(
      bind(socket_fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)),
      Not(Eq(-1)));

  // Call listen on the bound local socket.
  primitives::MessageWriter in;
  in.Push<int>(/*value=sockfd=*/socket_fd);
  in.Push<int>(/*value=backlog=*/8);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestListen, &in, &out));
  ASSERT_THAT(out, SizeIs(1));
  EXPECT_THAT(out.next<int>(), Eq(0));

  // Create another local socket and ensures that it is valid (fd > 0).
  int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_THAT(client_sock, Gt(0));

  // Attempt to connect the new socket to the local address. This call
  // will only succeed if the listen is successful.
  EXPECT_THAT(connect(client_sock, reinterpret_cast<struct sockaddr *>(&sa),
                      sizeof(sa)),
              Not(Eq(-1)));

  close(socket_fd);
  close(client_sock);
}

TEST_F(HostCallTest, TestShutdown) {
  // Create a local socket and ensure that it is valid (fd > 0).
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_THAT(socket_fd, Gt(0));

  std::string sockpath =
      absl::StrCat("/tmp/", absl::ToUnixNanos(absl::Now()), ".sock");

  // Create a local socket address and bind the socket to it.
  sockaddr_un sa = {};
  sa.sun_family = AF_UNIX;
  strncpy(&sa.sun_path[0], sockpath.c_str(), sizeof(sa.sun_path) - 1);
  EXPECT_THAT(
      bind(socket_fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)),
      Not(Eq(-1)));

  // Call shutdown on the bound local socket.
  primitives::MessageWriter in;
  in.Push<int>(/*value=sockfd=*/socket_fd);
  in.Push<int>(/*value=how=*/SHUT_RDWR);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestShutdown, &in, &out));
  ASSERT_THAT(out, SizeIs(1));
  EXPECT_THAT(out.next<int>(), Eq(0));

  std::string msg = "Hello world!";
  EXPECT_THAT(send(socket_fd, (void *)msg.c_str(), msg.length(), 0), Eq(-1));

  close(socket_fd);
}

TEST_F(HostCallTest, TestSend) {
  // Create a local socket and ensure that it is valid (fd > 0).
  int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_THAT(socket_fd, Gt(0));

  std::string sockpath =
      absl::StrCat("/tmp/", absl::ToUnixNanos(absl::Now()), ".sock");

  // Create a local socket address and bind the socket to it.
  sockaddr_un sa = {};
  sa.sun_family = AF_UNIX;
  strncpy(&sa.sun_path[0], sockpath.c_str(), sizeof(sa.sun_path) - 1);
  ASSERT_THAT(
      bind(socket_fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)),
      Not(Eq(-1)));

  ASSERT_THAT(listen(socket_fd, 8), Not(Eq(-1)));

  // Create another local socket and ensures that it is valid (fd > 0).
  int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  EXPECT_THAT(client_sock, Gt(0));

  // Attempt to connect the new socket to the local address. This call
  // will only succeed if the listen is successful.
  ASSERT_THAT(connect(client_sock, reinterpret_cast<struct sockaddr *>(&sa),
                      sizeof(sa)),
              Not(Eq(-1)));

  int connection_socket = accept(socket_fd, nullptr, nullptr);

  std::string msg = "Hello world!";

  primitives::MessageWriter in;
  in.Push<int>(/*value=sockfd=*/connection_socket);
  in.Push(/*value=buf*/ msg);
  in.Push<size_t>(/*value=len*/ msg.length());
  in.Push<int>(/*value=flags*/ 0);
  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestSend, &in, &out));
  ASSERT_THAT(out, SizeIs(1));
  EXPECT_THAT(out.next<int>(), Eq(msg.length()));

  close(socket_fd);
  close(client_sock);
  close(connection_socket);
}

// Tests enc_untrusted_fcntl() by performing various file control operations
// from inside the enclave and validating the return valueswith those obtained
// from native host call to fcntl().
TEST_F(HostCallTest, TestFcntl) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Get file flags and compare to those obtained from native fcntl() syscall.
  primitives::MessageWriter in;
  in.Push<int>(/*value=fd=*/fd);
  in.Push<int>(/*value=cmd=*/F_GETFL);
  in.Push<int>(/*value=arg=*/0);
  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFcntl, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.

  int fcntl_return;
  int klinux_fcntl_return = fcntl(fd, F_GETFL, 0);
  FromkLinuxFileStatusFlag(&klinux_fcntl_return, &fcntl_return);
  EXPECT_THAT(out.next<int>(), Eq(fcntl_return));

  // Turn on one or more of the file status flags for a descriptor.
  int flags_to_set = O_APPEND | O_NONBLOCK | O_RDONLY;
  primitives::MessageWriter in2;
  in2.Push<int>(/*value=fd=*/fd);
  in2.Push<int>(/*value=cmd=*/F_SETFL);
  in2.Push<int>(/*value=arg=*/flags_to_set);
  primitives::MessageReader out2;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFcntl, &in2, &out2));
  ASSERT_THAT(out2, SizeIs(1));  // Should only contain return value.

  klinux_fcntl_return = fcntl(fd, F_SETFL, flags_to_set);
  FromkLinuxFileStatusFlag(&klinux_fcntl_return, &fcntl_return);
  EXPECT_THAT(out2.next<int>(), Eq(fcntl_return));
}

TEST_F(HostCallTest, TestFcntlInvalidCmd) {
  primitives::MessageWriter in;
  in.Push<int>(/*value=fd=*/0);
  in.Push<int>(/*value=cmd=*/10000000);
  in.Push<int>(/*value=arg=*/0);
  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFcntl, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(-1));
}

// Tests enc_untrusted_chown() by attempting to change file ownership by making
// the host call from inside the enclave and verifying the return value.
TEST_F(HostCallTest, TestChown) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  primitives::MessageWriter in;
  in.Push(test_file);
  in.Push<uid_t>(/*value=owner=*/getuid());
  in.Push<gid_t>(/*value=group=*/getgid());

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestChown, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
}

// Tests enc_untrusted_fchown() by attempting to change file ownership by making
// the host call from inside the enclave and verifying the return value.
TEST_F(HostCallTest, TestFChown) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  struct stat sb = {};
  EXPECT_THAT(fstat(fd, &sb), Eq(0));
  EXPECT_THAT(sb.st_uid, Eq(getuid()));
  EXPECT_THAT(sb.st_gid, Eq(getgid()));

  primitives::MessageWriter in;
  in.Push<int>(/*value=fd=*/fd);
  in.Push<uid_t>(/*value=owner=*/getuid());
  in.Push<gid_t>(/*value=group=*/getgid());

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFChown, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));

  // Attempt to fchown with invalid file descriptor, should return an error.
  primitives::MessageWriter in2;
  in2.Push<int>(/*value=fd=*/-1);
  in2.Push<uid_t>(/*value=owner=*/getuid());
  in2.Push<gid_t>(/*value=group=*/getgid());

  primitives::MessageReader out2;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFChown, &in2, &out2));
  ASSERT_THAT(out2, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out2.next<int>(), Eq(-1));
}

// Tests enc_untrusted_setsockopt() by creating a socket on the untrusted side,
// passing the socket file descriptor to the trusted side, and invoking
// the host call for setsockopt() from inside the enclave. Verifies the return
// value obtained from the host call to confirm that the new options have been
// set.
TEST_F(HostCallTest, TestSetSockOpt) {
  // Create an TCP socket (SOCK_STREAM) with Internet Protocol Family AF_INET6.
  int socket_fd = socket(AF_INET6, SOCK_STREAM, 0);
  EXPECT_THAT(socket_fd, Gt(0));

  // Bind the TCP socket to port 0 for any IP address. Once bind is successful
  // for UDP sockets application can operate on the socket descriptor for
  // sending or receiving data.
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_flowinfo = 0;
  sa.sin6_addr = in6addr_any;
  sa.sin6_port = htons(0);
  EXPECT_THAT(
      bind(socket_fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)),
      Not(Eq(-1)));

  primitives::MessageWriter in;
  in.Push<int>(/*value=sockfd=*/socket_fd);
  in.Push<int>(/*value=level=*/SOL_SOCKET);
  in.Push<int>(/*value=optname=*/SO_REUSEADDR);
  in.Push<int>(/*value=option=*/1);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestSetSockOpt, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Gt(-1));

  close(socket_fd);
}

// Tests enc_untrusted_flock() by trying to acquire an exclusive lock on a valid
// file from inside the enclave by making the untrusted host call and verifying
// its return value. We do not validate if the locked file can be accessed from
// another process. A child process created using fork() would be able to access
// the file since both the processes refer to the same lock, and this lock may
// be modified or released by either processes, as specified in the man page for
// flock.
TEST_F(HostCallTest, TestFlock) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");

  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  int klinux_lock = LOCK_EX;
  int lock;
  FromkLinuxFLockOperation(&klinux_lock, &lock);
  primitives::MessageWriter in;
  in.Push<int>(/*value=fd=*/fd);
  in.Push<int>(/*value=operation=*/lock);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFlock, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
  flock(fd, LOCK_UN);
}

// Tests enc_untrusted_fsync by writing to a valid file, and then running fsync
// on it. Ensures that a successful code of 0 is returned.
TEST_F(HostCallTest, TestFsync) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Write something to the file.
  std::string file_content = "some random content.";
  ASSERT_THAT(write(fd, file_content.c_str(), file_content.length() + 1),
              Eq(file_content.length() + 1));

  primitives::MessageWriter in;
  in.Push<int>(/*value=fd*/ fd);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestFsync, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
}

// Tests enc_untrusted_inotify_init1() by initializing a new inotify instance
// from inside the enclave and verifying that a file descriptor associated with
// a new inotify event queue is returned. Only the return value, i.e. the file
// descriptor value is verified to be positive.
TEST_F(HostCallTest, TestInotifyInit1) {
  primitives::MessageWriter in;
  int inotify_flag;
  int klinux_inotify_flag = IN_NONBLOCK;
  FromkLinuxInotifyFlag(&klinux_inotify_flag, &inotify_flag);
  in.Push<int>(/*value=flags=*/inotify_flag);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestInotifyInit1, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  int inotify_fd = out.next<int>();
  EXPECT_THAT(inotify_fd, Gt(0));
  close(inotify_fd);
}

// Tests enc_untrusted_inotify_add_watch() by initializing an inotify instance
// on the untrusted side, making the enclave call to trigger an untrusted host
// call to inotify_add_watch(), and validating that the correct events are
// recorded in the event buffer for the folder we are monitoring with inotify.
TEST_F(HostCallTest, TestInotifyAddWatch) {
  int inotify_fd = inotify_init1(IN_NONBLOCK);
  ASSERT_THAT(inotify_fd, Gt(0));

  // Call inotify_add_watch from inside the enclave for monitoring tmpdir for
  // all events supported by inotify.
  primitives::MessageWriter in;
  in.Push<int>(inotify_fd);
  in.Push(absl::GetFlag(FLAGS_test_tmpdir));

  int event_mask;
  int klinux_event_mask = IN_ALL_EVENTS;
  FromkLinuxInotifyEventMask(&klinux_event_mask, &event_mask);
  in.Push<int>(event_mask);
  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestInotifyAddWatch, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(1));

  // Read the event buffer when no events have occurred in tmpdir.
  constexpr size_t event_size = sizeof(struct inotify_event);
  constexpr size_t buf_len = 10 * (event_size + NAME_MAX + 1);
  char buf[buf_len];
  EXPECT_THAT(read(inotify_fd, buf, buf_len), Eq(-1));

  // Perform an event by creating a file in tmpdir.
  std::string file_name = "test_file.tmp";
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/", file_name);
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Read the event buffer after the event.
  EXPECT_THAT(read(inotify_fd, buf, buf_len), Gt(0));

  auto *event = reinterpret_cast<struct inotify_event *>(&buf[0]);
  EXPECT_THAT(event->mask, Eq(IN_MODIFY));
  EXPECT_THAT(event->name, StrEq(file_name));
  EXPECT_THAT(event->cookie, Eq(0));

  event =
      reinterpret_cast<struct inotify_event *>(&buf[event_size + event->len]);
  EXPECT_THAT(event->mask, Eq(IN_OPEN));
  EXPECT_THAT(event->name, StrEq(file_name));
  EXPECT_THAT(event->cookie, Eq(0));

  close(inotify_fd);
}

// Tests enc_untrusted_inotify_rm_watch() by de-registering an event from inside
// the enclave on the untrusted side and verifying that subsequent activity
// on the unregistered event is not recorded by inotify.
TEST_F(HostCallTest, TestInotifyRmWatch) {
  int inotify_fd = inotify_init1(IN_NONBLOCK);
  std::string watch_dir = absl::GetFlag(FLAGS_test_tmpdir);
  int wd = inotify_add_watch(inotify_fd, watch_dir.c_str(), IN_ALL_EVENTS);
  ASSERT_THAT(inotify_fd, Gt(0));
  ASSERT_THAT(wd, Eq(1));

  // Perform an event by creating a file in tmpdir.
  std::string file_name = "test_file.tmp";
  std::string test_file = absl::StrCat(watch_dir, "/", file_name);
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  // Read the event buffer after the event.
  constexpr size_t event_size = sizeof(struct inotify_event);
  constexpr size_t buf_len = 10 * (event_size + NAME_MAX + 1);
  char buf[buf_len];
  EXPECT_THAT(read(inotify_fd, buf, buf_len), Gt(0));

  auto *event = reinterpret_cast<struct inotify_event *>(&buf[0]);
  EXPECT_THAT(event->mask, Eq(IN_MODIFY));
  EXPECT_THAT(event->name, StrEq(file_name));
  EXPECT_THAT(event->cookie, Eq(0));

  event =
      reinterpret_cast<struct inotify_event *>(&buf[event_size + event->len]);
  EXPECT_THAT(event->mask, Eq(IN_OPEN));
  EXPECT_THAT(event->name, StrEq(file_name));
  EXPECT_THAT(event->cookie, Eq(0));

  // Call inotify_rm_watch from inside the enclave, verify the return value.
  primitives::MessageWriter in;
  in.Push<int>(inotify_fd);
  in.Push<int>(wd);
  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestInotifyRmWatch, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));

  // Perform another event on the file.
  ASSERT_THAT(unlink(test_file.c_str()), Eq(0));

  // Read from the event buffer again to verify that the event was not recorded.
  EXPECT_THAT(read(inotify_fd, buf, buf_len), Gt(-1));
  close(inotify_fd);
}

// Tests enc_untrusted_sched_yield by calling it and ensuring that 0 is
// returned.
TEST_F(HostCallTest, TestSchedYield) {
  primitives::MessageWriter in;

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestSchedYield, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
}

// Tests enc_untrusted_isatty() by testing with a non-terminal file descriptor,
// it should return 0 since the file is not referring to a terminal.
TEST_F(HostCallTest, TestIsAtty) {
  std::string test_file =
      absl::StrCat(absl::GetFlag(FLAGS_test_tmpdir), "/test_file.tmp");
  int fd =
      open(test_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  platform::storage::FdCloser fd_closer(fd);
  ASSERT_GE(fd, 0);
  ASSERT_NE(access(test_file.c_str(), F_OK), -1);

  primitives::MessageWriter in;
  in.Push<int>(/*value=fd=*/fd);

  primitives::MessageReader out;
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestIsAtty, &in, &out));
  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
}

// Tests enc_untrusted_usleep() by sleeping for 1s, then ensuring that the
// return value is 0, and that at least 1 second passed during the usleep
// enclave call.
TEST_F(HostCallTest, TestUSleep) {
  primitives::MessageWriter in;

  // Push the sleep duration as unsigned int instead of useconds_t, storing
  // it as useconds_t causes a segfault when popping the argument from the
  // stack on the trusted side.
  in.Push<unsigned int>(/*value=usec=*/1000000);
  primitives::MessageReader out;

  absl::Time start = absl::Now();
  ASYLO_ASSERT_OK(client_->EnclaveCall(kTestUSleep, &in, &out));
  absl::Time end = absl::Now();

  auto duration = absl::ToInt64Milliseconds(end - start);

  ASSERT_THAT(out, SizeIs(1));  // Should only contain return value.
  EXPECT_THAT(out.next<int>(), Eq(0));
  EXPECT_GE(duration, 1000);
  EXPECT_LE(duration, 1200);
}

}  // namespace
}  // namespace host_call
}  // namespace asylo
