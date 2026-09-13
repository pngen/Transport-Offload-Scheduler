// Bridge fallback for builds without a CUDA toolkit.
//
// src/backends/cuda_backend.cpp reaches the device only through these functions, which
// are implemented by src/backends/cuda_kernels.cu when nvcc is available. This file is
// compiled instead when it is not, so that the accelerator backend exists, reports
// itself honestly as UNSUPPORTED, and never claims hardware it does not have.
//
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include <cstdint>

namespace {

constexpr int kNotCompiled = 1;  // must match TOS_CUDA_NOT_COMPILED in both peers
constexpr const char* kReason = "this build has no CUDA toolkit";

void report(char* error_out, int error_capacity) {
  if (error_out == nullptr || error_capacity <= 0) return;
  int index = 0;
  while (kReason[index] != '\0' && index < error_capacity - 1) {
    error_out[index] = kReason[index];
    ++index;
  }
  error_out[index] = '\0';
}

}  // namespace

extern "C" int tos_cuda_probe(int device_index, char* error_out, int error_capacity) {
  (void)device_index;
  report(error_out, error_capacity);
  return kNotCompiled;
}

extern "C" int tos_cuda_open(int device_index, char* error_out, int error_capacity) {
  (void)device_index;
  report(error_out, error_capacity);
  return kNotCompiled;
}

extern "C" void tos_cuda_close(void) {}

extern "C" int tos_cuda_device_name(int device_index, char* name_out, int name_capacity) {
  (void)device_index;
  (void)name_out;
  (void)name_capacity;
  return kNotCompiled;
}

extern "C" int tos_cuda_compute_capability(int device_index, int* major, int* minor) {
  (void)device_index;
  (void)major;
  (void)minor;
  return kNotCompiled;
}

extern "C" int tos_cuda_total_memory(int device_index, unsigned long long* bytes) {
  (void)device_index;
  (void)bytes;
  return kNotCompiled;
}

extern "C" int tos_cuda_driver_version(int* version) {
  (void)version;
  return kNotCompiled;
}

extern "C" int tos_cuda_runtime_version(int* version) {
  (void)version;
  return kNotCompiled;
}

extern "C" int tos_cuda_memory_info(unsigned long long* free_bytes, unsigned long long* total_bytes) {
  (void)free_bytes;
  (void)total_bytes;
  return kNotCompiled;
}

extern "C" unsigned long long tos_cuda_outstanding_bytes(void) { return 0; }

extern "C" unsigned long long tos_cuda_kernel_launches(void) { return 0; }

extern "C" unsigned long long tos_cuda_device_bytes(void) { return 0; }

extern "C" int tos_cuda_checksum_crc32c(const unsigned char* data, unsigned long long length,
                                        unsigned int* crc_out, unsigned long long* total_ns,
                                        unsigned long long* kernel_ns, char* error_out,
                                        int error_capacity) {
  (void)data;
  (void)length;
  (void)crc_out;
  (void)total_ns;
  (void)kernel_ns;
  report(error_out, error_capacity);
  return kNotCompiled;
}
