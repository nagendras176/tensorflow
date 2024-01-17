/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/nccl_api.h"

#include <cstddef>
#include <cstdint>

#include "absl/algorithm/container.h"
#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "third_party/nccl/nccl.h"
#include "xla/primitive_util.h"
#include "xla/service/gpu/nccl_clique_key.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/gpu_stream.h"
#include "xla/stream_executor/stream.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/statusor.h"

namespace xla::gpu {

//==-----------------------------------------------------------------------===//
// Macros to return or warn on NCCL errors.
//==-----------------------------------------------------------------------===//

static absl::Status ToStatus(ncclResult_t s, const char* file, int64_t line,
                             const char* expr) {
  if (s == ncclSuccess) return absl::OkStatus();

  return absl::InternalError(absl::StrFormat(
      "%s:%d: NCCL operation %s failed: %s."
      " Last NCCL warning(error) log entry (may be unrelated) '%s'.",
      file, line, expr, ncclGetErrorString(s), ncclGetLastError(nullptr)));
}

#define XLA_NCCL_STATUS(expr) \
  xla::gpu::ToStatus(expr, __FILE__, __LINE__, #expr)

#define XLA_NCCL_RETURN_IF_ERROR(expr)      \
  do {                                      \
    absl::Status s = XLA_NCCL_STATUS(expr); \
    if (!s.ok()) {                          \
      return s;                             \
    }                                       \
  } while (0)

#define XLA_NCCL_LOG_IF_ERROR(expr)         \
  do {                                      \
    absl::Status s = XLA_NCCL_STATUS(expr); \
    if (!s.ok()) {                          \
      LOG(ERROR) << s.ToString();           \
    }                                       \
  } while (0)

//==-----------------------------------------------------------------------===//
// Conversions between XLA and NCCL data types
//==-----------------------------------------------------------------------===//

static size_t ToNcclCount(PrimitiveType dtype, size_t count) {
  return primitive_util::IsComplexType(dtype) ? count * 2 : count;
}

static absl::StatusOr<ncclDataType_t> ToNcclDataType(PrimitiveType dtype,
                                                     bool is_reduction_op) {
  switch (dtype) {
    case S8:
    case F8E5M2:
    case F8E4M3FN:
      return ncclInt8;
    case PRED:
    case U8:
      return ncclUint8;
    case S32:
      return ncclInt32;
    case U32:
      return ncclUint32;
    case S64:
      return ncclInt64;
    case U64:
      return ncclUint64;
    case F16:
      return ncclFloat16;
    case F32:
    case C64:
      return ncclFloat32;
    case F64:
    case C128:
      return ncclFloat64;
    case S16:
    case U16:
      // For reductions we expect 16 bit integer types to be promoted to 32-bit.
      if (is_reduction_op) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Unsupported data type for reduction operation: %s",
                            primitive_util::LowercasePrimitiveTypeName(dtype)));
      }
      // For collectives that just move data around, we can use ncclFloat16 for
      // 16-bit integer data types.
      return ncclFloat16;
    case BF16:
      return ncclBfloat16;
    default:
      return absl::InvalidArgumentError(
          absl::StrFormat("Unsupported data type: %s",
                          primitive_util::LowercasePrimitiveTypeName(dtype)));
  }
}

//==-----------------------------------------------------------------------===//
// NcclApi
//==-----------------------------------------------------------------------===//

static_assert(NCCL_UNIQUE_ID_BYTES == NcclCliqueId::kSize,
              "size of nccl unique id must match the clique id size");

static NcclCommHandle Cast(ncclComm_t comm) {
  return reinterpret_cast<NcclCommHandle>(comm);
}

static ncclComm_t Cast(NcclCommHandle comm) {
  return reinterpret_cast<ncclComm_t>(comm);
}

static ncclUniqueId AsNcclUniqueId(const NcclCliqueId& clique_id) {
  ncclUniqueId id;
  absl::c_copy(clique_id.data(), id.internal);
  return id;
}

absl::StatusOr<NcclCliqueId> NcclApi::GetUniqueId() {
  VLOG(3) << "Get NCCL unique id";
  ncclUniqueId id;
  XLA_NCCL_RETURN_IF_ERROR(ncclGetUniqueId(&id));
  return NcclCliqueId(id.internal);
}

absl::StatusOr<NcclCommHandle> NcclApi::CommInitRank(
    int32_t nranks, const NcclCliqueId& clique_id, int32_t rank) {
  VLOG(1) << "Initialize NCCL communicator for rank #" << rank << " of "
          << nranks << "; hash(id)=" << absl::HashOf(clique_id.data());

  if (rank < 0 || rank >= nranks)
    return absl::InvalidArgumentError(absl::StrFormat(
        "Invalid rank %d, it must be in [0, %d) range", rank, nranks));

  ncclComm_t comm = nullptr;
  absl::Status status = XLA_NCCL_STATUS(
      ncclCommInitRank(&comm, nranks, AsNcclUniqueId(clique_id), rank));

  return Cast(comm);
}

absl::Status NcclApi::CommAbort(NcclCommHandle comm) {
  VLOG(1) << "Abort NCCL communicator: " << comm;
  return XLA_NCCL_STATUS(ncclCommAbort(Cast(comm)));
}

absl::StatusOr<int32_t> NcclApi::CommCount(NcclCommHandle comm) {
  VLOG(5) << "Get the number of ranks in NCCL communicator: " << comm;
  int32_t count;
  XLA_NCCL_RETURN_IF_ERROR(ncclCommCount(Cast(comm), &count));
  return count;
}

absl::Status NcclApi::CommGetAsyncError(NcclCommHandle comm) {
  VLOG(5) << "Get last async error for NCCL communicator: " << comm;

  ncclResult_t async_err;
  XLA_NCCL_RETURN_IF_ERROR(ncclCommGetAsyncError(Cast(comm), &async_err));
  if (async_err == ncclSuccess) return absl::OkStatus();

  return absl::InternalError(absl::StrCat(
      ncclGetErrorString(async_err),
      ". Last NCCL error (maybe unrelated): ", ncclGetLastError(Cast(comm))));
}

absl::Status NcclApi::GroupStart() {
  VLOG(5) << "Start NCCL group";
  return XLA_NCCL_STATUS(ncclGroupStart());
}

absl::Status NcclApi::GroupEnd() {
  VLOG(5) << "End NCCL group";
  return XLA_NCCL_STATUS(ncclGroupEnd());
}

absl::Status NcclApi::AllGather(se::DeviceMemoryBase send_buffer,
                                se::DeviceMemoryBase recv_buffer,
                                PrimitiveType dtype, size_t count,
                                NcclCommHandle comm, se::Stream* stream) {
  VLOG(3) << absl::StreamFormat(
      "Launch NCCL AllGather operation on device #%d; send_buffer=%p; "
      "recv_buffer=%p; dtype=%s; count=%d; comm=%p; stream=%p",
      stream->parent()->device_ordinal(), send_buffer.opaque(),
      recv_buffer.opaque(), primitive_util::LowercasePrimitiveTypeName(dtype),
      count, comm, stream);

  TF_ASSIGN_OR_RETURN(ncclDataType_t nccl_dtype, ToNcclDataType(dtype, false));

  return XLA_NCCL_STATUS(ncclAllGather(
      send_buffer.opaque(), recv_buffer.opaque(), ToNcclCount(dtype, count),
      nccl_dtype, Cast(comm), se::gpu::AsGpuStreamValue(stream)));
}

}  // namespace xla::gpu