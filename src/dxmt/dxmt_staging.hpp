#pragma once
#include "Metal.hpp"
#include "dxmt_buffer.hpp"
#include "thread.hpp"
#include <cstdint>
#include <atomic>
#include <queue>

namespace dxmt {

enum class StagingMapResult : uint64_t {
  Mappable = 0,
  Renamable = 0xffffffffffffffff,
  Mapped = 0xfffffffffffffffe,
};

class StagingResource {
public:
  void incRef();
  void decRef();

  void useCopyDestination(uint64_t seq_id);
  void useCopySource(uint64_t seq_id);

  StagingMapResult tryMap(uint64_t coherent_seq_id, bool read, bool write);
  void unmap();

  // Called only after Map's wait/rename loop selected the returned allocation.
  // READ maps do not turn GPU-produced bytes into CPU-owned replay inputs.
  void traceMap(bool write) {
    if (write) {
      trace_write_allocation_ = immediate_name_;
      buffer_pool[immediate_name_]->traceMappedWrite(0, length, WMTTraceOwnerBegin);
    }
  }

  uint64_t allocate(uint64_t coherent_seq_id);
  void updateImmediateName(uint64_t current_seq_id, uint64_t allocation);

  void *
  mappedImmediateMemory() {
    return buffer_pool[immediate_name_]->mappedMemory(0);
  }

  void updateImmediateContents(uint64_t offset, const void *data, uint64_t size) {
    buffer_pool[immediate_name_]->updateContents(offset, data, size);
  }

  void *
  mappedMemory(uint64_t id) {
    return buffer_pool[id]->mappedMemory(0);
  }

  Rc<Buffer> &
  buffer() {
    return buffer_;
  };

  Rc<BufferAllocation>
  allocation(uint64_t id) {
    return buffer_pool[id];
  };

  /**
  readonly
   */
  uint32_t bytesPerRow;
  /**
  readonly
   */
  uint32_t bytesPerImage;
  /**
  readonly
   */
  uint64_t length;

  StagingResource(WMT::Device device, uint64_t length, uint32_t bytes_per_row, uint32_t bytes_per_image);

private:
  struct QueueEntry {
    uint64_t id;
    uint64_t will_free_at;
  };

  Rc<Buffer> buffer_;
  uint64_t immediate_name_;
  std::vector<Rc<BufferAllocation>> buffer_pool;
  std::atomic<uint32_t> refcount_ = {0u};
  std::queue<QueueEntry> fifo;
  dxmt::mutex mutex_;
  bool mapped = false;
  uint64_t trace_write_allocation_ = ~0ull;
  // prevent read from staging before
  uint64_t cpu_coherent_after_finished_seq_id = 0;
  // prevent write to staging before
  uint64_t gpu_occupied_until_finished_seq_id = 0;
  WMT::Device device_;
};

} // namespace dxmt
