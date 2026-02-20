#pragma once

#if __has_include(<ESPBufferManager.h>)
#include <ESPBufferManager.h>
#define ESP_EVENTBUS_HAS_BUFFER_MANAGER 1
#elif __has_include(<esp_buffer_manager/buffer_manager.h>)
#include <esp_buffer_manager/buffer_manager.h>
#define ESP_EVENTBUS_HAS_BUFFER_MANAGER 1
#else
#define ESP_EVENTBUS_HAS_BUFFER_MANAGER 0
#endif

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <vector>

namespace eventbus_allocator_detail {
inline void* allocate(size_t bytes, bool usePSRAMBuffers) noexcept {
#if ESP_EVENTBUS_HAS_BUFFER_MANAGER
    return ESPBufferManager::allocate(bytes, usePSRAMBuffers);
#else
    (void)usePSRAMBuffers;
    return std::malloc(bytes);
#endif
}

inline void deallocate(void* ptr) noexcept {
#if ESP_EVENTBUS_HAS_BUFFER_MANAGER
    ESPBufferManager::deallocate(ptr);
#else
    std::free(ptr);
#endif
}
}  // namespace eventbus_allocator_detail

template <typename T>
class EventBusAllocator {
  public:
    using value_type = T;

    EventBusAllocator() noexcept = default;
    explicit EventBusAllocator(bool usePSRAMBuffers) noexcept : usePSRAMBuffers_(usePSRAMBuffers) {}

    template <typename U>
    EventBusAllocator(const EventBusAllocator<U>& other) noexcept : usePSRAMBuffers_(other.usePSRAMBuffers()) {}

    T* allocate(size_t count) {
        if (count == 0) {
            return nullptr;
        }
        if (count > (std::numeric_limits<size_t>::max() / sizeof(T))) {
            std::abort();
        }

        void* mem = eventbus_allocator_detail::allocate(count * sizeof(T), usePSRAMBuffers_);
        if (!mem) {
            std::abort();
        }
        return static_cast<T*>(mem);
    }

    void deallocate(T* ptr, size_t) noexcept {
        eventbus_allocator_detail::deallocate(ptr);
    }

    bool usePSRAMBuffers() const noexcept {
        return usePSRAMBuffers_;
    }

    template <typename U>
    bool operator==(const EventBusAllocator<U>& other) const noexcept {
        return usePSRAMBuffers_ == other.usePSRAMBuffers();
    }

    template <typename U>
    bool operator!=(const EventBusAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

  private:
    template <typename>
    friend class EventBusAllocator;

    bool usePSRAMBuffers_ = false;
};

template <typename T>
using EventBusVector = std::vector<T, EventBusAllocator<T>>;
