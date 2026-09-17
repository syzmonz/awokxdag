#pragma once

#include <esp_heap_caps.h>
#include <new>

// Application results are only accessed by loopTask. Keep driver callbacks,
// DMA buffers and their queues in internal RAM. Construct String members;
// allocating/zeroing raw bytes alone is not valid for these entry types.
template <typename T, size_t Count>
class MiniResultTable {
 public:
  bool initialize(size_t& externalBytes) {
    if (entries_) return true;
    void* storage = heap_caps_malloc(sizeof(T) * Count,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool external = storage != nullptr;
    if (!storage) {
      storage = heap_caps_malloc(sizeof(T) * Count,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!storage) return false;
    entries_ = static_cast<T*>(storage);
    for (size_t i = 0; i < Count; ++i) new (entries_ + i) T();
    if (external) externalBytes += sizeof(T) * Count;
    return true;
  }

  MiniResultTable() = default;
  MiniResultTable(const MiniResultTable&) = delete;
  MiniResultTable& operator=(const MiniResultTable&) = delete;
  ~MiniResultTable() {
    if (!entries_) return;
    for (size_t i = 0; i < Count; ++i) entries_[i].~T();
    heap_caps_free(entries_);
  }
  operator T*() { return entries_; }
  operator const T*() const { return entries_; }

 private:
  T* entries_ = nullptr;
};
