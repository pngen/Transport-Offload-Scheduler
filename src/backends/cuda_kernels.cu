// Real CUDA kernels, device context and private bridge for the accelerator backend.
//
// See include/tos/backends/cuda_backend.hpp for the behaviour this file implements.
//
// This file is compiled by nvcc only. It owns the process device context (device,
// primary context, stream, timing events) and the kernels, and it exposes them to
// src/backends/cuda_backend.cpp through the extern "C" bridge declared below. The bridge
// carries plain C types only, so the backend's host translation unit needs no CUDA
// header, no CUDA include path and no CUDA language support, and the rest of the
// project builds without a toolkit.
//
// The whole CUDA implementation is guarded by __CUDACC__. If this file is compiled by a
// host compiler instead, the #else branch defines the same entry points and reports
// "not compiled with CUDA" from every one of them: a build without a toolkit can never
// claim a real device.
//
// CRC32C on the device
// --------------------
// The byte work is a table-free bitwise CRC32C (reflected Castagnoli polynomial
// 0x82F63B78) with exactly the init/final convention of tos::crc32c: start from ~seed,
// fold eight branch-free shift/xor steps per byte, invert the register at the end.
//
// A CRC is a linear function of its data, so a buffer can be hashed in parallel through
// the standard identity
//   crc(A || B) = shift(crc(A), |B|) ^ crc(B),
// where shift(v, n) advances the CRC register over n zero bytes and is a GF(2) matrix
// product. Two levels of that identity are used:
//   * crc32c_chunk_kernel splits the buffer into up to kMaxChunks chunks; inside a chunk
//     every one of the kThreadsPerChunk threads hashes one contiguous slice and the
//     slices are then folded pairwise in shared memory (log2(threads) steps);
//   * crc32c_fold_kernel folds the per-chunk digests, plus the ragged tail chunk, into
//     the digest of the whole buffer.
// The shift matrices are powers of one operator built from the polynomial itself, and
// they are uploaded once into __constant__ memory as a table of 41 powers of two (one
// byte to 2^40 bytes). Uploading a constant polynomial table is not a shortcut: the
// table contains no data and no staged result, and every byte of the payload is hashed
// by the device.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.

/* Private bridge status codes. This block must stay identical to the copy in
   src/backends/cuda_backend.cpp: the two translation units are the only users of the
   bridge and there is deliberately no shared CUDA header. */
#define TOS_CUDA_OK 0
#define TOS_CUDA_NOT_COMPILED 1
#define TOS_CUDA_NO_DEVICE 2
#define TOS_CUDA_DRIVER_ERROR 3
#define TOS_CUDA_INVALID_DEVICE 4
#define TOS_CUDA_ALREADY_OPEN 5
#define TOS_CUDA_NOT_OPEN 6
#define TOS_CUDA_ALLOCATION_FAILED 7
#define TOS_CUDA_COPY_FAILED 8
#define TOS_CUDA_LAUNCH_FAILED 9
#define TOS_CUDA_SYNCHRONIZE_FAILED 10
#define TOS_CUDA_INVALID_ARGUMENT 11
#define TOS_CUDA_INTERNAL_ERROR 12

/* Private bridge. Plain C types only; definitions live in this file, callers live in
   src/backends/cuda_backend.cpp. Every function returns TOS_CUDA_OK on success. */
extern "C" int tos_cuda_probe(int device_index, char* error_out, int error_capacity);
extern "C" int tos_cuda_open(int device_index, char* error_out, int error_capacity);
extern "C" void tos_cuda_close(void);
extern "C" int tos_cuda_device_name(int device_index, char* name_out, int name_capacity);
extern "C" int tos_cuda_compute_capability(int device_index, int* major, int* minor);
extern "C" int tos_cuda_total_memory(int device_index, unsigned long long* bytes);
extern "C" int tos_cuda_driver_version(int* version);
extern "C" int tos_cuda_runtime_version(int* version);
extern "C" int tos_cuda_memory_info(unsigned long long* free_bytes, unsigned long long* total_bytes);
extern "C" unsigned long long tos_cuda_outstanding_bytes(void);
extern "C" unsigned long long tos_cuda_kernel_launches(void);
extern "C" unsigned long long tos_cuda_device_bytes(void);
extern "C" int tos_cuda_checksum_crc32c(const unsigned char* data, unsigned long long length,
                                        unsigned int* crc_out, unsigned long long* total_ns,
                                        unsigned long long* kernel_ns, char* error_out,
                                        int error_capacity);

#if defined(__CUDACC__)

#include <cuda_runtime.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace {

constexpr unsigned int kCrc32cPolynomial = 0x82F63B78u;  // reflected Castagnoli
constexpr unsigned int kCrc32cSeed = 0xFFFFFFFFu;        // ~0, the seed CRC of seed 0
constexpr int kThreadsPerChunk = 256;                    // threads folding one chunk
constexpr unsigned long long kMaxChunks = 128;           // parallel chunks per call
constexpr int kMaxOperatorBits = 41;                     // shifts up to 2^40 bytes
constexpr unsigned long long kMaxDeviceBytes = 1ULL << 40;

/// Powers of the one-byte shift operator: entry t advances a CRC register over 2^t zero
/// bytes. Filled once per process from the polynomial (see build_shift_operators) and
/// read by both kernels.
__constant__ unsigned int g_shift_operators[kMaxOperatorBits][32];

// ---------------------------------------------------------------------------
// GF(2) shift operators
// ---------------------------------------------------------------------------

/// Matrix product over GF(2): one row of a 32x32 bit matrix times a bit vector.
__device__ __forceinline__ unsigned int gf2_matrix_times(const unsigned int* matrix,
                                                         unsigned int vector) {
  unsigned int sum = 0u;
  int row = 0;
  while (vector != 0u) {
    if ((vector & 1u) != 0u) sum ^= matrix[row];
    vector >>= 1;
    ++row;
  }
  return sum;
}

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------

/// Fold eight zero bits of the polynomial shift into the register.
__device__ __forceinline__ unsigned int crc32c_bitwise(unsigned int crc, unsigned char byte) {
  crc ^= static_cast<unsigned int>(byte);
#pragma unroll
  for (int bit = 0; bit < 8; ++bit) {
    const unsigned int mask = 0u - (crc & 1u);
    crc = (crc >> 1) ^ (kCrc32cPolynomial & mask);
  }
  return crc;
}

/// Hash one contiguous range on one thread.
__device__ __forceinline__ unsigned int crc32c_range(const unsigned char* begin,
                                                     unsigned long long length) {
  unsigned int crc = kCrc32cSeed;
  for (unsigned long long offset = 0; offset < length; ++offset) {
    crc = crc32c_bitwise(crc, begin[offset]);
  }
  return crc;
}

/// One chunk per thread block. Block index < chunk_count covers the equal chunks; block
/// index chunk_count covers the ragged tail chunk when one exists. Each block writes the
/// standard CRC32C of its own bytes (init ~0, final inversion), which is exactly what
/// tos::crc32c returns for that slice.
__global__ void crc32c_chunk_kernel(const unsigned char* data, unsigned long long chunk_bytes,
                                    unsigned long long sub_bytes, unsigned int sub_shift,
                                    unsigned int chunk_count, unsigned long long tail_bytes,
                                    unsigned int* chunk_crcs) {
  __shared__ unsigned int accumulators[kThreadsPerChunk];
  // The fold matrix is staged through shared memory: the threads of a warp walk its rows
  // with different indices, and divergent constant-memory addresses are replayed one
  // address at a time, whereas shared memory serves them in parallel.
  __shared__ unsigned int fold_matrix[32];
  const unsigned int index = blockIdx.x;
  const int thread = threadIdx.x;
  if (index < chunk_count) {
    const unsigned char* base =
        data + static_cast<unsigned long long>(index) * chunk_bytes;
    if (sub_bytes == 0ULL) {
      // Payloads smaller than one block of slices: one thread hashes the whole chunk.
      if (thread == 0) chunk_crcs[index] = ~crc32c_range(base, chunk_bytes);
      return;
    }
    // Every thread hashes one contiguous slice of the chunk...
    const unsigned char* slice = base + static_cast<unsigned long long>(thread) * sub_bytes;
    accumulators[thread] = ~crc32c_range(slice, sub_bytes);
    __syncthreads();
    // ... and the slices are folded pairwise with a distance-doubling tree. CRC folding
    // is order dependent, so the tree only ever joins *adjacent* ranges: at level l
    // thread t holds the range of sub_bytes << l bytes starting at t * (sub_bytes << l)
    // and joins it with the range held by thread t + stride, whose length is exactly
    // sub_bytes << l bytes. That is a shift by 2^(sub_shift + l) bytes.
    int level = 0;
    for (int stride = 1; stride < kThreadsPerChunk; stride <<= 1) {
      if (thread < 32) {
        fold_matrix[thread] = g_shift_operators[static_cast<int>(sub_shift) + level][thread];
      }
      __syncthreads();
      if (thread % (2 * stride) == 0) {
        accumulators[thread] =
            gf2_matrix_times(fold_matrix, accumulators[thread]) ^ accumulators[thread + stride];
      }
      ++level;
      __syncthreads();
    }
    if (thread == 0) chunk_crcs[index] = accumulators[0];
    return;
  }
  if (tail_bytes != 0ULL && thread == 0) {
    const unsigned char* base =
        data + static_cast<unsigned long long>(chunk_count) * chunk_bytes;
    chunk_crcs[chunk_count] = ~crc32c_range(base, tail_bytes);
  }
}

/// Fold the per-chunk digests plus the tail digest into the digest of the whole buffer.
/// One block, one thread: this is a few thousand XORs, not a data-parallel loop.
__global__ void crc32c_fold_kernel(const unsigned int* chunk_crcs, unsigned int chunk_count,
                                   unsigned int chunk_shift, unsigned long long tail_bytes,
                                   unsigned int* result) {
  if (chunk_count == 0u) {
    *result = 0u;
    return;
  }
  if (chunk_count == 1u && tail_bytes == 0ULL) {
    // Single chunk: the chunk digest is the answer. This also covers an empty input,
    // whose chunk digest is ~kCrc32cSeed = 0, the CRC32C of zero bytes.
    *result = chunk_crcs[0];
    return;
  }
  // crc(A || B) = shift(crc(A), |B|) ^ crc(B), applied left to right over equal chunks.
  unsigned int acc = chunk_crcs[0];
  for (unsigned int index = 1; index < chunk_count; ++index) {
    acc = gf2_matrix_times(g_shift_operators[static_cast<int>(chunk_shift)], acc) ^
          chunk_crcs[index];
  }
  if (tail_bytes != 0ULL) {
    unsigned long long remaining = tail_bytes;
    int bit = 0;
    while (remaining != 0ULL) {
      if ((remaining & 1ULL) != 0ULL) {
        acc = gf2_matrix_times(g_shift_operators[bit], acc);
      }
      remaining >>= 1;
      ++bit;
    }
    acc ^= chunk_crcs[chunk_count];
  }
  *result = acc;
}

// ---------------------------------------------------------------------------
// Host-side operator table
// ---------------------------------------------------------------------------

unsigned int host_matrix_times(const unsigned int* matrix, unsigned int vector) {
  unsigned int sum = 0u;
  int row = 0;
  while (vector != 0u) {
    if ((vector & 1u) != 0u) sum ^= matrix[row];
    vector >>= 1;
    ++row;
  }
  return sum;
}

void host_matrix_square(unsigned int* square, const unsigned int* matrix) {
  for (int row = 0; row < 32; ++row) {
    square[row] = host_matrix_times(matrix, matrix[row]);
  }
}

/// Build the constant table: entry 0 advances one byte, entry t advances 2^t bytes.
void build_shift_operators(unsigned int table[kMaxOperatorBits][32]) {
  unsigned int one_bit[32];
  one_bit[0] = kCrc32cPolynomial;
  unsigned int row = 1u;
  for (int index = 1; index < 32; ++index) {
    one_bit[index] = row;
    row <<= 1;
  }
  unsigned int two_bits[32];
  host_matrix_square(two_bits, one_bit);
  host_matrix_square(one_bit, two_bits);       // four bits
  host_matrix_square(table[0], one_bit);       // eight bits: one byte
  for (int bit = 1; bit < kMaxOperatorBits; ++bit) {
    host_matrix_square(table[bit], table[bit - 1]);
  }
}

/// Round a byte count up to a four-byte boundary.
unsigned long long align4(unsigned long long value) { return (value + 3ULL) & ~3ULL; }

/// Exponent of a power of two (the callers only pass powers of two).
unsigned int shift_exponent(unsigned long long value) {
  unsigned int exponent = 0;
  unsigned long long power = 1;
  while (power < value) {
    power <<= 1;
    ++exponent;
  }
  return exponent;
}

// ---------------------------------------------------------------------------
// Device context and accounting
// ---------------------------------------------------------------------------

struct device_context {
  bool open{false};
  int device_index{-1};
  cudaStream_t stream{nullptr};
  cudaEvent_t kernel_start{nullptr};
  cudaEvent_t kernel_stop{nullptr};
};

std::mutex& device_mutex() {
  static std::mutex mutex;
  return mutex;
}

device_context& context() {
  static device_context instance;
  return instance;
}

std::atomic<unsigned long long> g_outstanding_bytes{0};
std::atomic<unsigned long long> g_kernel_launches{0};
std::atomic<unsigned long long> g_device_bytes_copied{0};

/// Bounded string builder: the bridge never allocates and never throws.
struct error_text {
  char* out{nullptr};
  int capacity{0};
  int position{0};

  void add(const char* text) {
    if (out == nullptr || capacity <= 0) return;
    for (int index = 0; text[index] != '\0' && position < capacity - 1; ++index) {
      out[position++] = text[index];
    }
    out[position] = '\0';
  }
  void add_cuda(cudaError_t status) {
    add(cudaGetErrorName(status));
    add(": ");
    add(cudaGetErrorString(status));
  }
  void add_failure(const char* what, cudaError_t status) {
    add(what);
    add_cuda(status);
  }
};

void clear_error(char* out, int capacity) {
  if (out != nullptr && capacity > 0) out[0] = '\0';
}

/// Scope guard over one CUDA allocation. Both the byte accounting and the release are
/// unconditional, so no error path can leak device memory or pinned host memory.
struct allocation_guard {
  void* pointer{nullptr};
  unsigned long long bytes{0};
  bool pinned{false};

  allocation_guard() = default;
  allocation_guard(const allocation_guard&) = delete;
  allocation_guard& operator=(const allocation_guard&) = delete;
  ~allocation_guard() { release(); }

  void release() {
    if (pointer == nullptr) return;
    if (pinned) {
      (void)cudaFreeHost(pointer);
    } else {
      (void)cudaFree(pointer);
    }
    g_outstanding_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    pointer = nullptr;
    bytes = 0;
  }
};

cudaError_t allocate_device(allocation_guard& guard, unsigned long long bytes) {
  void* pointer = nullptr;
  const cudaError_t status = cudaMalloc(&pointer, static_cast<std::size_t>(bytes));
  if (status != cudaSuccess) return status;
  guard.pointer = pointer;
  guard.bytes = bytes;
  guard.pinned = false;
  g_outstanding_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return cudaSuccess;
}

cudaError_t allocate_pinned(allocation_guard& guard, unsigned long long bytes) {
  void* pointer = nullptr;
  const cudaError_t status = cudaMallocHost(&pointer, static_cast<std::size_t>(bytes));
  if (status != cudaSuccess) return status;
  guard.pointer = pointer;
  guard.bytes = bytes;
  guard.pinned = true;
  g_outstanding_bytes.fetch_add(bytes, std::memory_order_relaxed);
  return cudaSuccess;
}

/// Enumerate, range-check and fully initialise the requested device. Idempotent and free
/// of lasting state: this is what "available()" means.
int probe_device(int device_index, error_text& error) {
  int count = 0;
  cudaError_t status = cudaGetDeviceCount(&count);
  if (status != cudaSuccess) {
    error.add_failure("cudaGetDeviceCount failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }
  if (count <= 0) {
    error.add("no CUDA device is present on this host");
    return TOS_CUDA_NO_DEVICE;
  }
  if (device_index < 0 || device_index >= count) {
    error.add("CUDA device index is out of range");
    return TOS_CUDA_INVALID_DEVICE;
  }
  status = cudaSetDevice(device_index);
  if (status != cudaSuccess) {
    error.add_failure("cudaSetDevice failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }
  // Forces the primary context to be created now, so an unusable driver is reported
  // here instead of in the middle of an execution.
  status = cudaFree(nullptr);
  if (status != cudaSuccess) {
    error.add_failure("CUDA initialisation failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }
  return TOS_CUDA_OK;
}

/// Shared body of the device fact queries: they must answer for any index without
/// disturbing the execution context.
int query_properties(int device_index, cudaDeviceProp& properties, error_text& error) {
  const int probed = probe_device(device_index, error);
  if (probed != TOS_CUDA_OK) return probed;
  const cudaError_t status = cudaGetDeviceProperties(&properties, device_index);
  if (status != cudaSuccess) {
    error.add_failure("cudaGetDeviceProperties failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }
  return TOS_CUDA_OK;
}

}  // namespace

// ---------------------------------------------------------------------------
// Bridge implementation
// ---------------------------------------------------------------------------

extern "C" int tos_cuda_probe(int device_index, char* error_out, int error_capacity) {
  clear_error(error_out, error_capacity);
  error_text error;
  error.out = error_out;
  error.capacity = error_capacity;
  std::lock_guard<std::mutex> lock(device_mutex());
  return probe_device(device_index, error);
}

extern "C" int tos_cuda_open(int device_index, char* error_out, int error_capacity) {
  clear_error(error_out, error_capacity);
  error_text error;
  error.out = error_out;
  error.capacity = error_capacity;
  std::lock_guard<std::mutex> lock(device_mutex());
  device_context& ctx = context();
  if (ctx.open) {
    if (ctx.device_index == device_index) return TOS_CUDA_OK;
    error.add("this process already owns a CUDA context on another device index");
    return TOS_CUDA_ALREADY_OPEN;
  }
  const int probed = probe_device(device_index, error);
  if (probed != TOS_CUDA_OK) return probed;

  // One-time constant upload of the shift operators. This is a symbol write, not an
  // allocation, so it never appears in the outstanding-bytes accounting.
  unsigned int operators[kMaxOperatorBits][32];
  build_shift_operators(operators);
  cudaError_t status =
      cudaMemcpyToSymbol(g_shift_operators, operators, sizeof(operators), 0, cudaMemcpyHostToDevice);
  if (status != cudaSuccess) {
    error.add_failure("cudaMemcpyToSymbol failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }

  status = cudaStreamCreateWithFlags(&ctx.stream, cudaStreamNonBlocking);
  if (status != cudaSuccess) {
    error.add_failure("cudaStreamCreateWithFlags failed: ", status);
    return TOS_CUDA_DRIVER_ERROR;
  }
  status = cudaEventCreateWithFlags(&ctx.kernel_start, cudaEventDefault);
  if (status == cudaSuccess) status = cudaEventCreateWithFlags(&ctx.kernel_stop, cudaEventDefault);
  if (status != cudaSuccess) {
    error.add_failure("cudaEventCreateWithFlags failed: ", status);
    if (ctx.kernel_start != nullptr) {
      (void)cudaEventDestroy(ctx.kernel_start);
      ctx.kernel_start = nullptr;
    }
    (void)cudaStreamDestroy(ctx.stream);
    ctx.stream = nullptr;
    return TOS_CUDA_DRIVER_ERROR;
  }
  ctx.device_index = device_index;
  ctx.open = true;
  return TOS_CUDA_OK;
}

extern "C" void tos_cuda_close(void) {
  std::lock_guard<std::mutex> lock(device_mutex());
  device_context& ctx = context();
  if (!ctx.open) return;
  if (ctx.kernel_stop != nullptr) {
    (void)cudaEventDestroy(ctx.kernel_stop);
    ctx.kernel_stop = nullptr;
  }
  if (ctx.kernel_start != nullptr) {
    (void)cudaEventDestroy(ctx.kernel_start);
    ctx.kernel_start = nullptr;
  }
  if (ctx.stream != nullptr) {
    (void)cudaStreamDestroy(ctx.stream);
    ctx.stream = nullptr;
  }
  // The primary context itself is process-wide and shared with anything else in this
  // process that uses CUDA, so it is deliberately not reset here.
  ctx.open = false;
  ctx.device_index = -1;
}

extern "C" int tos_cuda_device_name(int device_index, char* name_out, int name_capacity) {
  if (name_out == nullptr || name_capacity <= 0) return TOS_CUDA_INVALID_ARGUMENT;
  name_out[0] = '\0';
  error_text error;
  std::lock_guard<std::mutex> lock(device_mutex());
  cudaDeviceProp properties{};
  const int status = query_properties(device_index, properties, error);
  if (status != TOS_CUDA_OK) return status;
  int position = 0;
  while (properties.name[position] != '\0' && position < name_capacity - 1) {
    name_out[position] = properties.name[position];
    ++position;
  }
  name_out[position] = '\0';
  return TOS_CUDA_OK;
}

extern "C" int tos_cuda_compute_capability(int device_index, int* major, int* minor) {
  if (major == nullptr || minor == nullptr) return TOS_CUDA_INVALID_ARGUMENT;
  error_text error;
  std::lock_guard<std::mutex> lock(device_mutex());
  cudaDeviceProp properties{};
  const int status = query_properties(device_index, properties, error);
  if (status != TOS_CUDA_OK) return status;
  *major = properties.major;
  *minor = properties.minor;
  return TOS_CUDA_OK;
}

extern "C" int tos_cuda_total_memory(int device_index, unsigned long long* bytes) {
  if (bytes == nullptr) return TOS_CUDA_INVALID_ARGUMENT;
  error_text error;
  std::lock_guard<std::mutex> lock(device_mutex());
  cudaDeviceProp properties{};
  const int status = query_properties(device_index, properties, error);
  if (status != TOS_CUDA_OK) return status;
  *bytes = static_cast<unsigned long long>(properties.totalGlobalMem);
  return TOS_CUDA_OK;
}

extern "C" int tos_cuda_driver_version(int* version) {
  if (version == nullptr) return TOS_CUDA_INVALID_ARGUMENT;
  const cudaError_t status = cudaDriverGetVersion(version);
  if (status != cudaSuccess) return TOS_CUDA_DRIVER_ERROR;
  return TOS_CUDA_OK;
}

extern "C" int tos_cuda_runtime_version(int* version) {
  if (version == nullptr) return TOS_CUDA_INVALID_ARGUMENT;
  const cudaError_t status = cudaRuntimeGetVersion(version);
  if (status != cudaSuccess) return TOS_CUDA_DRIVER_ERROR;
  return TOS_CUDA_OK;
}

extern "C" int tos_cuda_memory_info(unsigned long long* free_bytes, unsigned long long* total_bytes) {
  if (free_bytes == nullptr || total_bytes == nullptr) return TOS_CUDA_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lock(device_mutex());
  if (!context().open) return TOS_CUDA_NOT_OPEN;
  std::size_t free_memory = 0;
  std::size_t total_memory = 0;
  const cudaError_t status = cudaMemGetInfo(&free_memory, &total_memory);
  if (status != cudaSuccess) return TOS_CUDA_DRIVER_ERROR;
  *free_bytes = static_cast<unsigned long long>(free_memory);
  *total_bytes = static_cast<unsigned long long>(total_memory);
  return TOS_CUDA_OK;
}

extern "C" unsigned long long tos_cuda_outstanding_bytes(void) {
  return g_outstanding_bytes.load(std::memory_order_relaxed);
}

extern "C" unsigned long long tos_cuda_kernel_launches(void) {
  return g_kernel_launches.load(std::memory_order_relaxed);
}

extern "C" unsigned long long tos_cuda_device_bytes(void) {
  return g_device_bytes_copied.load(std::memory_order_relaxed);
}

extern "C" int tos_cuda_checksum_crc32c(const unsigned char* data, unsigned long long length,
                                        unsigned int* crc_out, unsigned long long* total_ns,
                                        unsigned long long* kernel_ns, char* error_out,
                                        int error_capacity) {
  clear_error(error_out, error_capacity);
  error_text error;
  error.out = error_out;
  error.capacity = error_capacity;
  if (crc_out == nullptr || total_ns == nullptr) {
    error.add("crc_out and total_ns must not be null");
    return TOS_CUDA_INVALID_ARGUMENT;
  }
  if (data == nullptr && length != 0ULL) {
    error.add("input pointer is null for a non-empty payload");
    return TOS_CUDA_INVALID_ARGUMENT;
  }
  if (length > kMaxDeviceBytes) {
    error.add("payload exceeds the device path bound");
    return TOS_CUDA_INVALID_ARGUMENT;
  }
  *crc_out = 0u;
  *total_ns = 0ULL;
  if (kernel_ns != nullptr) *kernel_ns = 0ULL;

  const auto started = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(device_mutex());
  device_context& ctx = context();
  if (!ctx.open) {
    error.add("the CUDA device context is not open");
    return TOS_CUDA_NOT_OPEN;
  }

  // Chunking: equal chunks of a power-of-two byte length, each split over one block of
  // kThreadsPerChunk slices, plus at most one ragged tail chunk.
  unsigned long long chunk_bytes = static_cast<unsigned long long>(kThreadsPerChunk);
  if (length == 0ULL) {
    chunk_bytes = 0ULL;
  } else {
    while (chunk_bytes * kMaxChunks < length) chunk_bytes <<= 1;
  }
  unsigned long long chunk_count = chunk_bytes == 0ULL ? 1ULL : length / chunk_bytes;
  unsigned long long tail_bytes = chunk_bytes == 0ULL ? 0ULL : length % chunk_bytes;
  unsigned long long sub_bytes = 0ULL;
  unsigned int sub_shift = 0;
  unsigned int chunk_shift = 0;
  if (chunk_count == 0ULL) {
    // Smaller than one chunk: one block, one thread, the whole payload.
    chunk_count = 1ULL;
    chunk_bytes = length;
    tail_bytes = 0ULL;
  } else if (chunk_bytes != 0ULL) {
    sub_bytes = chunk_bytes / static_cast<unsigned long long>(kThreadsPerChunk);
    sub_shift = shift_exponent(sub_bytes);
    chunk_shift = shift_exponent(chunk_bytes);
  }
  const unsigned long long tail_slots = tail_bytes == 0ULL ? 0ULL : 1ULL;
  const unsigned long long chunk_slots = chunk_count + tail_slots;
  const unsigned long long device_input_bytes = length == 0ULL ? 1ULL : length;
  const unsigned long long chunk_table_bytes = chunk_slots * sizeof(unsigned int);

  // Two allocations per execution, not five: one pinned host block holding the staged
  // input and the copied-back result, and one device block holding the input image, the
  // per-chunk digest table and the folded result. Driver allocator round trips dominate a
  // 1 MiB device call, and two allocations keep the accounting trivially exact.
  const unsigned long long pinned_result_offset = align4(device_input_bytes);
  const unsigned long long pinned_total_bytes = pinned_result_offset + sizeof(unsigned int);
  const unsigned long long device_chunks_offset = align4(device_input_bytes);
  const unsigned long long device_result_offset = device_chunks_offset + align4(chunk_table_bytes);
  const unsigned long long device_total_bytes = device_result_offset + sizeof(unsigned int);

  allocation_guard pinned_block;
  allocation_guard device_block;
  cudaError_t status = allocate_pinned(pinned_block, pinned_total_bytes);
  if (status == cudaSuccess) status = allocate_device(device_block, device_total_bytes);
  if (status != cudaSuccess) {
    error.add_failure("device allocation failed: ", status);
    return TOS_CUDA_ALLOCATION_FAILED;
  }

  unsigned char* const pinned_input = static_cast<unsigned char*>(pinned_block.pointer);
  unsigned char* const pinned_result = pinned_input + pinned_result_offset;
  unsigned char* const device_base = static_cast<unsigned char*>(device_block.pointer);
  unsigned char* const device_input = device_base;
  unsigned char* const device_chunks = device_base + device_chunks_offset;
  unsigned char* const device_result = device_base + device_result_offset;

  // Real staging through pinned host memory, then a real host-to-device copy.
  if (length != 0ULL) std::memcpy(pinned_input, data, static_cast<std::size_t>(length));
  status = cudaMemcpyAsync(device_input, pinned_input, static_cast<std::size_t>(length),
                           cudaMemcpyHostToDevice, ctx.stream);
  if (status != cudaSuccess) {
    error.add_failure("host-to-device copy failed: ", status);
    return TOS_CUDA_COPY_FAILED;
  }
  g_device_bytes_copied.fetch_add(length, std::memory_order_relaxed);

  status = cudaEventRecord(ctx.kernel_start, ctx.stream);
  if (status != cudaSuccess) {
    error.add_failure("cudaEventRecord failed: ", status);
    return TOS_CUDA_INTERNAL_ERROR;
  }

  crc32c_chunk_kernel<<<static_cast<unsigned int>(chunk_slots), kThreadsPerChunk, 0, ctx.stream>>>(
      device_input, chunk_bytes, sub_bytes, sub_shift, static_cast<unsigned int>(chunk_count),
      tail_bytes, reinterpret_cast<unsigned int*>(device_chunks));
  cudaError_t launch = cudaGetLastError();
  if (launch != cudaSuccess) {
    error.add_failure("crc32c_chunk_kernel launch failed: ", launch);
    return TOS_CUDA_LAUNCH_FAILED;
  }
  g_kernel_launches.fetch_add(1, std::memory_order_relaxed);

  crc32c_fold_kernel<<<1, 1, 0, ctx.stream>>>(reinterpret_cast<const unsigned int*>(device_chunks),
                                              static_cast<unsigned int>(chunk_count), chunk_shift,
                                              tail_bytes,
                                              reinterpret_cast<unsigned int*>(device_result));
  launch = cudaGetLastError();
  if (launch != cudaSuccess) {
    error.add_failure("crc32c_fold_kernel launch failed: ", launch);
    return TOS_CUDA_LAUNCH_FAILED;
  }
  g_kernel_launches.fetch_add(1, std::memory_order_relaxed);

  status = cudaEventRecord(ctx.kernel_stop, ctx.stream);
  if (status != cudaSuccess) {
    error.add_failure("cudaEventRecord failed: ", status);
    return TOS_CUDA_INTERNAL_ERROR;
  }
  status = cudaMemcpyAsync(pinned_result, device_result, sizeof(unsigned int),
                           cudaMemcpyDeviceToHost, ctx.stream);
  if (status != cudaSuccess) {
    error.add_failure("device-to-host copy failed: ", status);
    return TOS_CUDA_COPY_FAILED;
  }

  // The single synchronisation point of an execution: everything above is enqueued,
  // nothing above is assumed to have finished before this returns.
  status = cudaStreamSynchronize(ctx.stream);
  if (status != cudaSuccess) {
    error.add_failure("cudaStreamSynchronize failed: ", status);
    return TOS_CUDA_SYNCHRONIZE_FAILED;
  }
  g_device_bytes_copied.fetch_add(sizeof(unsigned int), std::memory_order_relaxed);

  if (kernel_ns != nullptr) {
    float elapsed_ms = 0.0F;
    if (cudaEventElapsedTime(&elapsed_ms, ctx.kernel_start, ctx.kernel_stop) == cudaSuccess) {
      *kernel_ns = static_cast<unsigned long long>(static_cast<double>(elapsed_ms) * 1000000.0);
    }
  }
  *crc_out = *reinterpret_cast<unsigned int*>(pinned_result);
  const auto finished = std::chrono::steady_clock::now();
  *total_ns = static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  // Every allocation is released by the scope guards on the way out.
  return TOS_CUDA_OK;
}

#else  // !defined(__CUDACC__)

// Compiled without nvcc: there are no kernels and no device. Every entry point says so
// instead of pretending, so the backend reports Provenance::kUnsupported.

namespace {

void write_not_compiled(char* out, int capacity) {
  if (out == nullptr || capacity <= 0) return;
  const char text[] = "cuda_kernels.cu was compiled without a CUDA toolkit";
  int index = 0;
  while (text[index] != '\0' && index < capacity - 1) {
    out[index] = text[index];
    ++index;
  }
  out[index] = '\0';
}

}  // namespace

extern "C" int tos_cuda_probe(int, char* error_out, int error_capacity) {
  write_not_compiled(error_out, error_capacity);
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_open(int, char* error_out, int error_capacity) {
  write_not_compiled(error_out, error_capacity);
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" void tos_cuda_close(void) {}

extern "C" int tos_cuda_device_name(int, char* name_out, int name_capacity) {
  write_not_compiled(name_out, name_capacity);
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_compute_capability(int, int* major, int* minor) {
  if (major != nullptr) *major = 0;
  if (minor != nullptr) *minor = 0;
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_total_memory(int, unsigned long long* bytes) {
  if (bytes != nullptr) *bytes = 0ULL;
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_driver_version(int* version) {
  if (version != nullptr) *version = 0;
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_runtime_version(int* version) {
  if (version != nullptr) *version = 0;
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" int tos_cuda_memory_info(unsigned long long* free_bytes, unsigned long long* total_bytes) {
  if (free_bytes != nullptr) *free_bytes = 0ULL;
  if (total_bytes != nullptr) *total_bytes = 0ULL;
  return TOS_CUDA_NOT_COMPILED;
}

extern "C" unsigned long long tos_cuda_outstanding_bytes(void) { return 0ULL; }

extern "C" unsigned long long tos_cuda_kernel_launches(void) { return 0ULL; }

extern "C" unsigned long long tos_cuda_device_bytes(void) { return 0ULL; }

extern "C" int tos_cuda_checksum_crc32c(const unsigned char*, unsigned long long, unsigned int* crc_out,
                                        unsigned long long* total_ns, unsigned long long* kernel_ns,
                                        char* error_out, int error_capacity) {
  write_not_compiled(error_out, error_capacity);
  if (crc_out != nullptr) *crc_out = 0u;
  if (total_ns != nullptr) *total_ns = 0ULL;
  if (kernel_ns != nullptr) *kernel_ns = 0ULL;
  return TOS_CUDA_NOT_COMPILED;
}

#endif  // defined(__CUDACC__)
