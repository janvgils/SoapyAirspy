#pragma once

#include <cstdint>
#include <cstddef>
#include <sys/mman.h>

#include <unistd.h>

#include <iostream>
#include <cstring>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>
#include <cassert>

#include <atomic>

// The Cortex-A7 in the RPi3 has a 64-byte cache line size (L2 cache)
#define cacheline_aligned alignas(64)

template <typename T>
class RingBuffer {

    T* buffer_;
    cacheline_aligned const uint32_t capacity_;
    cacheline_aligned std::atomic<uint32_t> read_pos_{0};
    cacheline_aligned std::atomic<uint32_t> write_pos_{0};

    cacheline_aligned uint32_t read_pos_cached_{0};
    cacheline_aligned uint32_t write_pos_cached_{0};

    cacheline_aligned uint32_t available_cached_{0};
    cacheline_aligned uint32_t free_cached_{0};

    cacheline_aligned std::mutex lock_;
    cacheline_aligned std::condition_variable cond_;

    // Unmap mirror memory
    static void unmap_mirror(const void *addr, const size_t size) noexcept {
        int munmap_res;
        munmap_res = munmap(const_cast<void *>(addr), 2 * size);
        if(munmap_res == -1) {
            std::cerr << "munmap failed" << std::endl;
        }
    }

    static bool is_power_of_two(const uint32_t val) {
        return (val != 0) && ((val & (val - 1)) == 0);
    }

    // Double mapped memory for "magic" ringbuffer.
    static T* map_mirror(const uint32_t size) {
        // Get pagesize
        const uint32_t pagesize = getpagesize();
        if(size < pagesize) {
            throw std::runtime_error("Capacity must be at least pagesize: "
                                     + std::to_string(pagesize)
                                     + " requested size: " + std::to_string(size));
        }

        if(!is_power_of_two(size)) {
            throw std::runtime_error("Capacity must be a power of two, requested size: "
                                     + std::to_string(size));
        }

        // Create a memfd. Name is only for debugging purposes and can
        // be reused.
        int mem_fd = memfd_create("soapy_ring_buffer", 0);
        if(mem_fd == -1) {
            // TODO: add excact error
            throw std::runtime_error("Could not create memfd: "
                                     + std::string(strerror(errno)));
        }

        // Truncate to size
        const int ftruncate_res = ftruncate(mem_fd, size);
        if(ftruncate_res == -1) {
            throw std::runtime_error("Could not ftruncate memfd: "
                                     + std::string(strerror(errno)));
        }

        // Find a piece of memory of size 2 * size.
        void* buffer = mmap(NULL, 2 * size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (buffer == MAP_FAILED) {
            throw std::runtime_error("Could not mmap buffer: "
                                     + std::string(strerror(errno)));
        }

        // Map the memfd to the first half of the buffer
        void* addr_hint_1 = buffer;
        void* buffer_1 = mmap(addr_hint_1, size,
                              PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                              mem_fd, 0);

        if (buffer_1 == MAP_FAILED or buffer_1 != addr_hint_1) {
            throw std::runtime_error("Could not mmap buffer1: "
                                     + std::string(strerror(errno)));
        }

        // Map the memfd to the second half of the buffer
        // The double cast is because of pointer arithmetic
        // TODO: fix this cast into a macro or something
        void* addr_hint_2 = (void*)((uint8_t*)buffer_1 + size);
        const void* buffer_2 = mmap(addr_hint_2, size,
                                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                                    mem_fd, 0);

        // Check if the second mmap was successful and that address is correct.
        if (buffer_2 == MAP_FAILED or buffer_2 != addr_hint_2) {
            throw std::runtime_error("Could not mmap buffer2: "
                                     + std::string(strerror(errno)));
        }

        // Probably not strictly neccassry.
        std::memset(buffer, 0, size);

        // All correct, return a pointer to the buffer
        return static_cast<T*>(buffer);
    }

    // Mask read and write pointer
    inline uint32_t mask(const uint32_t val) const noexcept {
        return (capacity_ - 1) & val;
    }

public:

    // Size in bytes, is a power of two.
    size_t size() const noexcept {
        return capacity_ * sizeof(T);
    }

    // Capacity in elements
    inline uint32_t capacity() const noexcept {
        return capacity_;
    }

    // Indicate number of elements written. Must only be called from
    // producer.
    inline void produce(const uint32_t elements) noexcept {
        // intentional wrap around arithmetic
        free_cached_ -= elements;
        write_pos_cached_ += elements;
        write_pos_.fetch_add(elements, std::memory_order_release);
        cond_.notify_one();
    }

    // Indicate number of elements read. Must only be called from
    // consumer.
    inline void consume(const uint32_t elements) noexcept {
        // intentional wrap around arithmetic
        available_cached_ -= elements;
        read_pos_cached_ += elements;
        read_pos_.fetch_add(elements, std::memory_order_release);
        cond_.notify_one();
    }

    // Available elements to read
    inline uint32_t available(const uint32_t required = 0) noexcept {
        if(available_cached_ < required) {
            available_cached_ = write_pos_.load(std::memory_order_acquire) - read_pos_cached_;
        }
        return available_cached_;
    }

    // Available space to write
    inline uint32_t free(const uint32_t required = 0) noexcept {
        if(free_cached_ < required) {
            free_cached_= capacity_ - (write_pos_cached_ - read_pos_.load(std::memory_order_acquire));
        }
        return free_cached_;
    }

    // Pointer to read location. Must only be called from consumer.
    inline const T* read_ptr() const noexcept {
        return buffer_ + mask(read_pos_cached_);
    }

    // Pointer to write location. Must only be called from producer.
    inline T* write_ptr() const noexcept {
        return buffer_ + mask(write_pos_cached_);
    }

    // Rest buffer
    void clear() noexcept {
        std::unique_lock<std::mutex> lock(lock_);
        available_cached_ = 0;
        free_cached_ = capacity_;
        read_pos_cached_ = 0;
        write_pos_cached_ = 0;
        read_pos_.store(0, std::memory_order_release);
        write_pos_.store(0, std::memory_order_release);

        // wake up producer
        // no need to wake up consumer since there's nothing to consume
        //consumed_cond_.notify_all();
        cond_.notify_all();
    }

    int32_t read_at_least(const uint32_t elements,
                          const std::chrono::microseconds& timeout,
                          const std::function<uint32_t(const T* begin, const uint32_t avail)> callback) {

        uint32_t avail_ = available(elements);
        if(avail_ >= elements) {
            // We have enough elements, no need to wait
            uint32_t consumed =  callback(read_ptr(), avail_);
            consume(consumed);
            return consumed;
        }

        // Else wait for more elements
        std::unique_lock<std::mutex> lock(lock_);
        // Wait for enough data to be available
        if(cond_.wait_for(lock, timeout, [&] {
            avail_ = available(elements);
            return avail_ >= elements;
        }))
        {
            // Ok, we have enough data
            uint32_t consumed = callback(read_ptr(), avail_);
            consume(consumed);
            return consumed;
        } else {
            // Timeout
            return -1;
        }
    }


    int32_t write_at_least(const uint32_t elements,
                           const std::chrono::microseconds& timeout,
                           const std::function<uint32_t(T* begin, const uint32_t free)> callback) {

        uint32_t free_ = free(elements);

        if(free_ >= elements) {
            // Ok, we have enough space, not need to wait
            uint32_t produced = callback(write_ptr(), free_);
            produce(produced);
            return produced;
        }

        // Else wait for enough space to be available
        std::unique_lock<std::mutex> lock(lock_);
        // Wait for enough data to be available
        if(cond_.wait_for(lock, timeout, [&] {
            free_ = free(elements);
            return free_ >= elements;
        }))
        {
            // Ok, we have enough data
            uint32_t produced = callback(write_ptr(), free_);
            produce(produced);
            return produced;
        } else {
            // Timeout
            return -1;
        }
    }

    RingBuffer(uint32_t capacity)
        : buffer_(map_mirror(capacity * sizeof(T))),
          capacity_(capacity) {
    }

    virtual ~RingBuffer() {
        unmap_mirror(buffer_, capacity_ * sizeof(T));
    };

};
