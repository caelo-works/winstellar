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

    buffer_.resize(want_bytes);

    size_t offset = 0;
    while (offset < buffer_.size()) {
        const ULONG chunk = static_cast<ULONG>(
            (buffer_.size() - offset) > 0x100000 ? 0x100000 : (buffer_.size() - offset));
        ULONG got = 0;
        hr = stream->Read(buffer_.data() + offset, chunk, &got);
        if (FAILED(hr)) { buffer_.clear(); total_size_ = 0; return hr; }
        if (got == 0) break;
        offset += got;
    }
    if (offset != buffer_.size()) {
        buffer_.resize(offset);
    }
    return S_OK;
}
