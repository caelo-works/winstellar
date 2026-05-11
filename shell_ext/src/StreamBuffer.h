#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Reads up to N bytes from an IStream into a contiguous memory buffer.
// PropertyHandler uses a small cap (header-only mode for huge XISF), while
// Preview/Thumbnail handlers ask for everything up to the hard cap.
class StreamBuffer {
public:
    static constexpr size_t kHardCap   = 1024ull * 1024 * 1024;   // 1 GB max
    static constexpr size_t kHeaderCap = 32ull   * 1024 * 1024;   // 32 MB - enough
                                                                  // for any real
                                                                  // XISF XML header
                                                                  // and any sane FITS

    HRESULT init(IStream* stream, size_t max_bytes = kHardCap);
    void clear() noexcept { buffer_.clear(); buffer_.shrink_to_fit(); total_size_ = 0; }

    const uint8_t* data() const noexcept { return buffer_.data(); }
    size_t size() const noexcept { return buffer_.size(); }
    bool empty() const noexcept { return buffer_.empty(); }
    // Total stream size from Stat (may exceed buffer_.size() when capped).
    uint64_t total_size() const noexcept { return total_size_; }

private:
    std::vector<uint8_t> buffer_;
    uint64_t total_size_ = 0;
};
