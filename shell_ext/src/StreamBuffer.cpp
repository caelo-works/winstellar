#include "StreamBuffer.h"

#include <algorithm>

HRESULT StreamBuffer::init(IStream* stream, size_t max_bytes) {
    if (!stream) return E_POINTER;

    STATSTG stat = {};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;

    total_size_ = stat.cbSize.QuadPart;
    if (total_size_ == 0) return E_INVALIDARG;

    const size_t want_bytes = static_cast<size_t>(
        std::min<uint64_t>(total_size_, max_bytes));

    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    // Uninitialized allocation: the loop below overwrites every byte read,
    // so zero-fill would only matter on the short-read tail (handled by
    // not advertising those bytes via size_).
    buffer_.reset(new uint8_t[want_bytes]);
    size_ = 0;

    while (size_ < want_bytes) {
        const ULONG remaining = static_cast<ULONG>(want_bytes - size_);
        const ULONG chunk = (remaining > 0x100000) ? 0x100000u : remaining;
        ULONG got = 0;
        hr = stream->Read(buffer_.get() + size_, chunk, &got);
        if (FAILED(hr)) { clear(); return hr; }
        if (got == 0) break;
        size_ += got;
    }
    return S_OK;
}
