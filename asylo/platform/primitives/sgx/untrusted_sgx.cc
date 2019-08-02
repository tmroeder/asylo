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

#include "asylo/platform/primitives/sgx/untrusted_sgx.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>

#include "asylo/platform/common/bridge_types.h"
#include "asylo/platform/primitives/sgx/sgx_error_space.h"
#include "asylo/platform/primitives/sgx/sgx_params.h"
#include "asylo/platform/primitives/untrusted_primitives.h"
#include "asylo/platform/primitives/util/message.h"
#include "asylo/util/cleanup.h"
#include "asylo/util/elf_reader.h"
#include "asylo/util/file_mapping.h"
#include "asylo/util/status.h"
#include "asylo/util/status_macros.h"
#include "asylo/util/statusor.h"
#include "include/sgx_edger8r.h"
#include "include/sgx_urts.h"

// Edger8r-generated ocall table.
struct ocall_table_t {
  size_t nr_ocall;
  void *table[];
};

// This global is written into at compile time by the untrusted bridge files
// generated by edger8r.
extern "C" ABSL_CONST_INIT const ocall_table_t ocall_table_bridge;

namespace asylo {
namespace primitives {

namespace {

constexpr absl::string_view kCallingProcessBinaryFile = "/proc/self/exe";

constexpr int kMaxEnclaveCreateAttempts = 5;

// Edger8r-generated primitives ecall marshalling struct.
struct ms_ecall_dispatch_trusted_call_t {
  // Return value from the trusted call.
  int ms_retval;

  // Trusted selector value.
  uint64_t ms_selector;

  // Pointer to the flat buffer passed to primitives::EnclaveCall. The
  // pointer is interpreted as a void pointer as edger8r only allows trivial
  // data types to be passed across the bridge.
  void *ms_buffer;
};

// Edger8r-generated primitives ecall_deliver_signal marshalling struct.
struct ms_ecall_deliver_signal_t {
  // Return value from the trusted call.
  int ms_retval;

  // Pointer to the flat buffer passed to primitives::EnclaveCall. The
  // pointer is interpreted as a void pointer as edger8r only allows trivial
  // data types to be passed across the bridge.
  void *ms_buffer;
};

// Edger8r-generated primitives ms_ecall_transfer_secure_snapshot_key_t
// marshalling struct.
struct ms_ecall_transfer_secure_snapshot_key_t {
  int ms_retval;
  const char *ms_input;
  bridge_size_t ms_input_len;
  char **ms_output;
  bridge_size_t *ms_output_len;
};

// Enters the enclave and invokes the secure snapshot key transfer entry-point.
// If the ecall fails, return a non-OK status.
static Status TransferSecureSnapshotKey(sgx_enclave_id_t eid, const char *input,
                                        size_t input_len, char **output,
                                        size_t *output_len) {
  bridge_size_t bridge_output_len;
  ms_ecall_transfer_secure_snapshot_key_t ms;
  ms.ms_input = input;
  ms.ms_input_len = input_len;
  ms.ms_output = output;
  ms.ms_output_len = &bridge_output_len;
  sgx_status_t sgx_status = sgx_ecall(eid, 4, &ocall_table_bridge, &ms, true);
  if (output_len) {
    *output_len = static_cast<size_t>(bridge_output_len);
  }
  if (sgx_status != SGX_SUCCESS) {
    // Return a Status object in the SGX error space.
    return Status(sgx_status, "Call to ecall_do_handshake failed");
  } else if (ms.ms_retval || *output_len == 0) {
    // Ecall succeeded but did not return a value. This indicates that the
    // trusted code failed to propagate error information over the enclave
    // boundary.
    return Status(error::GoogleError::INTERNAL, "No output from enclave");
  }
  return Status::OkStatus();
}

// Edger8r-generated primitives ecall_take_snapshot marshalling struct.
struct ms_ecall_take_snapshot_t {
  int ms_retval;
  char **ms_output;
  bridge_size_t *ms_output_len;
};

// Enters the enclave and invokes the snapshotting entry-point. If the ecall
// fails, return a non-OK status.
static Status TakeSnapshot(sgx_enclave_id_t eid, char **output,
                           size_t *output_len) {
  bridge_size_t bridge_output_len;
  ms_ecall_take_snapshot_t ms;
  ms.ms_output = output;
  ms.ms_output_len = &bridge_output_len;
  sgx_status_t sgx_status = sgx_ecall(eid, 2, &ocall_table_bridge, &ms, true);

  if (output_len) {
    *output_len = static_cast<size_t>(bridge_output_len);
  }
  if (sgx_status != SGX_SUCCESS) {
    // Return a Status object in the SGX error space.
    return Status(sgx_status, "Call to ecall_take_snapshot failed");
  } else if (ms.ms_retval || *output_len == 0) {
    // Ecall succeeded but did not return a value. This indicates that the
    // trusted code failed to propagate error information over the enclave
    // boundary.
    return Status(asylo::error::GoogleError::INTERNAL,
                  "No output from enclave");
  }

  return Status::OkStatus();
}
}  // namespace

SgxEnclaveClient::~SgxEnclaveClient() = default;

StatusOr<std::shared_ptr<Client>> SgxBackend::Load(
    const absl::string_view enclave_name, void *base_address,
    absl::string_view enclave_path, size_t enclave_size,
    const EnclaveConfig &config, bool debug,
    std::unique_ptr<Client::ExitCallProvider> exit_call_provider) {
  std::shared_ptr<SgxEnclaveClient> client(
      new SgxEnclaveClient(enclave_name, std::move(exit_call_provider)));
  client->base_address_ = base_address;

  int updated;
  sgx_status_t status;
  const uint32_t ex_features = SGX_CREATE_ENCLAVE_EX_ASYLO;
  asylo_sgx_config_t create_config = {
      .base_address = &client->base_address_,
      .enclave_size = enclave_size,
      .enable_user_utility = config.enable_fork()};
  const void *ex_features_p[32] = {nullptr};
  ex_features_p[SGX_CREATE_ENCLAVE_EX_ASYLO_BIT_IDX] = &create_config;
  for (int i = 0; i < kMaxEnclaveCreateAttempts; ++i) {
    status = sgx_create_enclave_ex(
        std::string(enclave_path).c_str(), debug, &client->token_, &updated,
        &client->id_, /*misc_attr=*/nullptr, ex_features, ex_features_p);

    LOG_IF(WARNING, status != SGX_SUCCESS)
        << "Failed to create an enclave, attempt=" << i
        << ", status=" << status;
    if (status != SGX_INTERNAL_ERROR_ENCLAVE_CREATE_INTERRUPTED) {
      break;
    }
  }

  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to create an enclave");
  }

  client->size_ = sgx_enclave_size(client->id_);
  client->is_destroyed_ = false;
  return client;
}

StatusOr<std::shared_ptr<Client>> SgxEmbeddedBackend::Load(
    const absl::string_view enclave_name, void *base_address,
    absl::string_view section_name, size_t enclave_size,
    const EnclaveConfig &config, bool debug,
    std::unique_ptr<Client::ExitCallProvider> exit_call_provider) {
  std::shared_ptr<SgxEnclaveClient> client(
      new SgxEnclaveClient(enclave_name, std::move(exit_call_provider)));
  client->base_address_ = base_address;

  // If an address is specified to load the enclave, temporarily reserve it to
  // prevent these mappings from occupying that location.
  if (base_address && enclave_size > 0 &&
      mmap(base_address, enclave_size, PROT_NONE, MAP_SHARED | MAP_ANONYMOUS,
           -1, 0) != base_address) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to reserve enclave memory");
  }

  FileMapping self_binary_mapping;
  ASYLO_ASSIGN_OR_RETURN(self_binary_mapping, FileMapping::CreateFromFile(
                                                  kCallingProcessBinaryFile));

  ElfReader self_binary_reader;
  ASYLO_ASSIGN_OR_RETURN(self_binary_reader, ElfReader::CreateFromSpan(
                                                 self_binary_mapping.buffer()));

  absl::Span<const uint8_t> enclave_buffer;
  ASYLO_ASSIGN_OR_RETURN(enclave_buffer,
                         self_binary_reader.GetSectionData(section_name));

  if (base_address && enclave_size > 0 &&
      munmap(base_address, enclave_size) < 0) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to release enclave memory");
  }

  sgx_status_t status;
  const uint32_t ex_features = SGX_CREATE_ENCLAVE_EX_ASYLO;
  asylo_sgx_config_t create_config = {
      .base_address = &client->base_address_,
      .enclave_size = enclave_size,
      .enable_user_utility = config.enable_fork()};
  const void *ex_features_p[32] = {nullptr};
  ex_features_p[SGX_CREATE_ENCLAVE_EX_ASYLO_BIT_IDX] = &create_config;
  for (int i = 0; i < kMaxEnclaveCreateAttempts; ++i) {
    status = sgx_create_enclave_from_buffer_ex(
        const_cast<uint8_t *>(enclave_buffer.data()), enclave_buffer.size(),
        debug, &client->id_, /*misc_attr=*/nullptr, ex_features, ex_features_p);

    if (status != SGX_INTERNAL_ERROR_ENCLAVE_CREATE_INTERRUPTED) {
      break;
    }
  }

  client->size_ = sgx_enclave_size(client->id_);

  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to create an enclave");
  }

  client->is_destroyed_ = false;
  return client;
}

Status SgxEnclaveClient::Destroy() {
  MessageReader output;
  ASYLO_RETURN_IF_ERROR(EnclaveCall(kSelectorAsyloFini, nullptr, &output));
  sgx_status_t status = sgx_destroy_enclave(id_);
  if (status != SGX_SUCCESS) {
    return Status(status, "Failed to destroy enclave");
  }
  is_destroyed_ = true;
  return Status::OkStatus();
}

sgx_enclave_id_t SgxEnclaveClient::GetEnclaveId() const { return id_; }

size_t SgxEnclaveClient::GetEnclaveSize() const { return size_; }

void *SgxEnclaveClient::GetBaseAddress() const { return base_address_; }

void SgxEnclaveClient::GetLaunchToken(sgx_launch_token_t *token) const {
  memcpy(token, &token_, sizeof(token_));
}

bool SgxEnclaveClient::IsClosed() const { return is_destroyed_; }

Status SgxEnclaveClient::EnclaveCallInternal(uint64_t selector,
                                             MessageWriter *input,
                                             MessageReader *output) {
  if (is_destroyed_) {
    return Status{error::GoogleError::FAILED_PRECONDITION,
                  "Cannot make an enclave call to a closed enclave."};
  }

  ms_ecall_dispatch_trusted_call_t ms;
  ms.ms_selector = selector;
  SgxParams params;
  ms.ms_buffer = &params;

  params.input_size = 0;
  params.input = nullptr;
  params.output = nullptr;
  params.output_size = 0;
  Cleanup clean_up([&params] {
    if (params.input) {
      free(const_cast<void *>(params.input));
    }
    if (params.output) {
      free(params.output);
    }
  });

  if (input) {
    params.input_size = input->MessageSize();
    if (params.input_size > 0) {
      params.input = malloc(static_cast<size_t>(params.input_size));
      input->Serialize(const_cast<void *>(params.input));
    }
  }
  const ocall_table_t *table = &ocall_table_bridge;
  sgx_status_t status =
      sgx_ecall(id_, /*index=*/0, table, &ms, /*is_utility=*/false);
  if (status != SGX_SUCCESS) {
    // Return a Status object in the SGX error space.
    return Status(status, "Call to primitives ecall endpoint failed");
  }
  if (ms.ms_retval) {
    return Status(error::GoogleError::INTERNAL,
                  "Enclave call failed inside enclave");
  }
  if (params.output) {
    output->Deserialize(params.output, static_cast<size_t>(params.output_size));
  }
  return Status::OkStatus();
}

Status SgxEnclaveClient::DeliverSignalInternal(MessageWriter *input,
                                               MessageReader *output) {
  if (is_destroyed_) {
    return Status{error::GoogleError::FAILED_PRECONDITION,
                  "Cannot make an enclave call to a closed enclave."};
  }

  ms_ecall_deliver_signal_t ms;
  SgxParams params;
  ms.ms_buffer = &params;

  params.input_size = 0;
  params.input = nullptr;
  Cleanup clean_up([&params] {
    if (params.input) {
      free(const_cast<void *>(params.input));
    }
  });

  if (input) {
    params.input_size = input->MessageSize();
    if (params.input_size > 0) {
      params.input = malloc(static_cast<size_t>(params.input_size));
      input->Serialize(const_cast<void *>(params.input));
    }
  }
  const ocall_table_t *table = &ocall_table_bridge;
  sgx_status_t status =
      sgx_ecall(id_, /*index=*/1, table, &ms, /*is_utility=*/false);

  if (status != SGX_SUCCESS) {
    // Return a Status object in the SGX error space.
    return Status(status, "Call to primitives deliver signal endpoint failed");
  } else if (ms.ms_retval) {
    std::string message;
    switch (ms.ms_retval) {
      case 1:
        message = "Invalid or unregistered incoming signal";
        break;
      case 2:
        message = "Enclave unable to handle signal in current state";
        break;
      case -1:
        message = "Incoming signal is blocked inside the enclave";
        break;
      default:
        message = "Unexpected error while handling signal";
    }
    return Status(error::GoogleError::INTERNAL, message);
  }
  return Status::OkStatus();
}

Status SgxEnclaveClient::EnterAndTakeSnapshot(SnapshotLayout *snapshot_layout) {
  char *output_buf = nullptr;
  size_t output_len = 0;

  ASYLO_RETURN_IF_ERROR(TakeSnapshot(id_, &output_buf, &output_len));

  // Enclave entry-point was successfully invoked. |output_buf| is guaranteed to
  // have a value.
  EnclaveOutput local_output;
  local_output.ParseFromArray(output_buf, output_len);
  Status status;
  status.RestoreFrom(local_output.status());

  // If |output| is not null, then |output_buf| points to a memory buffer
  // allocated inside the enclave using
  // TrustedPrimitives::UntrustedLocalAlloc(). It is the caller's responsibility
  // to free this buffer.
  free(output_buf);

  // Set the output parameter if necessary.
  if (snapshot_layout) {
    *snapshot_layout = local_output.GetExtension(snapshot);
  }

  return status;
}

Status SgxEnclaveClient::EnterAndTransferSecureSnapshotKey(
    const ForkHandshakeConfig &fork_handshake_config) {
  std::string buf;
  if (!fork_handshake_config.SerializeToString(&buf)) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Failed to serialize ForkHandshakeConfig");
  }

  char *output = nullptr;
  size_t output_len = 0;

  ASYLO_RETURN_IF_ERROR(TransferSecureSnapshotKey(id_, buf.data(), buf.size(),
                                                  &output, &output_len));

  // Enclave entry-point was successfully invoked. |output| is guaranteed to
  // have a value.
  StatusProto status_proto;
  status_proto.ParseFromArray(output, output_len);
  Status status;
  status.RestoreFrom(status_proto);

  // |output| points to an untrusted memory buffer allocated by the enclave. It
  // is the untrusted caller's responsibility to free this buffer.
  free(output);

  return status;
}

bool SgxEnclaveClient::IsTcsActive() { return (sgx_is_tcs_active(id_) != 0); }

void SgxEnclaveClient::SetProcessId() { sgx_set_process_id(id_); }

}  // namespace primitives
}  // namespace asylo
