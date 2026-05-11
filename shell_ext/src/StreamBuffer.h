#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// Reads up to N bytes from an IStream into a contiguous memory buffer.
// PropertyHandler uses a small cap (header-only mode for huge XISF), while
// Preview/Thumbnail handlers ask for everything up to the hard cap.
//
// Backed by a unique_ptr<uint8_t[]> rather than std::vector so the allocation
// is uninitialized -- saves up to ~300 ms of pointless zero-fill on a 1 GB
// preview buffer that the IStream::Read is about to overwrite anyway.
class StreamBuffer {
public:
    static constexpr size_t kHardCap   = 1024ull * 1024 * 1024;   // 1 GB max
    static constexpr size_t kHeaderCap = 32ull   * 1024 * 1024;   // 32 MB - enough
                                                                  // for any real
                                                                  // XISF XML header
                                                                  // and any sane FITS

    HRESULT init(IStream* stream, size_t max_bytes = kHardCap);
    void clear() noexcept { buffer_.reset(); size_ = 0; total_size_ = 0; }

    const uint8_t* data() const noexcept { return buffer_.get(); }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    // Total stream size from Stat (may exceed size() when capped).
    uint64_t total_size() const noexcept { return total_size_; }

private:
    std::unique_ptr<uint8_t[]> buffer_;
    size_t   size_       = 0;
    uint64_t total_size_ = 0;
};
