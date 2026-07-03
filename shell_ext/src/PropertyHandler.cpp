#include "PropertyHandler.h"
#include "Guids.h"

#include "fits_core/fits_loader.h"
#include "fits_core/fits_headers.h"
#include "fits_core/xisf_loader.h"
#include "fits_core/raw_loader.h"
#include "fits_core/analysis.h"
#include "fits_core/cache.h"

#include <propvarutil.h>

#include <cmath>
#include <cstdio>
#include <new>
#include <string>

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                   static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        // Fallback: ASCII passthrough
        return std::wstring(s.begin(), s.end());
    }
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          w.data(), n);
    return w;
}

void add_string(std::vector<std::pair<PROPERTYKEY, PROPVARIANT>>& v,
                REFPROPERTYKEY key, const std::wstring& value) {
    if (value.empty()) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(::InitPropVariantFromString(value.c_str(), &pv))) {
        v.emplace_back(key, pv);
    } else {
        PropVariantClear(&pv);
    }
}

void add_double(std::vector<std::pair<PROPERTYKEY, PROPVARIANT>>& v,
                REFPROPERTYKEY key, double value) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_R8;
    pv.dblVal = value;
    v.emplace_back(key, pv);
}

// Round to N decimal places. Windows' default Double formatter uses the
// shortest round-trip representation, so a clean rounded value displays with
// at most N decimals (2.35 -> "2.35", 60.0 -> "60"). The propdesc schema has
// no attribute to control numeric precision for Doubles, so this is the
// pragmatic workaround.
inline double round_n(double v, int decimals) {
    if (!std::isfinite(v)) return v;
    const double s = std::pow(10.0, decimals);
    return std::round(v * s) / s;
}

void add_uint(std::vector<std::pair<PROPERTYKEY, PROPVARIANT>>& v,
              REFPROPERTYKEY key, ULONG value) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI4;
    pv.ulVal = value;
    v.emplace_back(key, pv);
}

void add_filetime(std::vector<std::pair<PROPERTYKEY, PROPVARIANT>>& v,
                  REFPROPERTYKEY key, int64_t ticks) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_FILETIME;
    pv.filetime.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    pv.filetime.dwHighDateTime = static_cast<DWORD>((ticks >> 32) & 0xFFFFFFFFu);
    v.emplace_back(key, pv);
}

}  // namespace

FitsPropertyHandler::FitsPropertyHandler() { DllAddRef(); }

FitsPropertyHandler::~FitsPropertyHandler() {
    clear_props();
    DllRelease();
}

void FitsPropertyHandler::clear_props() {
    for (auto& p : props_) PropVariantClear(&p.second);
    props_.clear();
}

HRESULT FitsPropertyHandler::CreateInstance(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* p = new (std::nothrow) FitsPropertyHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

IFACEMETHODIMP FitsPropertyHandler::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
        *ppv = static_cast<IInitializeWithStream*>(this);
    } else if (riid == IID_IPropertyStore) {
        *ppv = static_cast<IPropertyStore*>(this);
    } else if (riid == IID_IPropertyStoreCapabilities) {
        *ppv = static_cast<IPropertyStoreCapabilities*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) FitsPropertyHandler::AddRef() { return InterlockedIncrement(&ref_); }

IFACEMETHODIMP_(ULONG) FitsPropertyHandler::Release() {
    const ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
}

IFACEMETHODIMP FitsPropertyHandler::Initialize(IStream* stream, DWORD /*grfMode*/) {
    // Be tolerant of re-initialization (some hosts recycle handlers).
    clear_props();
    populated_ = false;
    buf_.clear();
    // Header-only read: enough for any XISF XML header (max ~17 MB observed)
    // and for the primary HDU of any sane FITS file. Avoids reading 850 MB
    // of pixel data per file when Explorer scrolls a folder of huge XISF
    // masters.
    HRESULT hr = buf_.init(stream, StreamBuffer::kHeaderCap);
    if (FAILED(hr)) return hr;
    populate();
    populated_ = true;
    return S_OK;
}

void FitsPropertyHandler::populate() {
    // Defensive: this runs in-process inside explorer.exe and SearchIndexer.exe.
    // Catch every conceivable failure path to keep Explorer alive.
    try {
        // Format-specific path. For FITS we need to round-trip through CFITSIO
        // to get the headers + pixel data. For XISF the XML header carries
        // everything we need for column emission and we DELIBERATELY skip
        // pixel decoding (the buffer is intentionally truncated to 32 MB
        // header-only mode by Initialize()).
        fitsx::FitsImage img;
        // Set true only when we hold enough pixel data to compute HFR/stars in
        // place. XISF/RAW stay header-only (lookup-only); FITS computes when the
        // whole file fits in the 32 MB buffer (see the Fits case below).
        bool can_run_analysis = false;

        switch (fitsx::detect_format(buf_.data(), buf_.size())) {
        case fitsx::ImageFormat::Xisf: {
            auto h = fitsx::parse_xisf_header(buf_.data(), buf_.size());
            if (!h.success) return;
            img.width = h.header.width;
            img.height = h.header.height;
            img.headers = std::move(h.header.fits_keywords);
            // No pixel data -> no analysis. Cache miss path won't run.
            break;
        }
        case fitsx::ImageFormat::Raw: {
            // Camera RAW: pull EXIF only (no demosaic). Decoding a 24 Mpx
            // frame just to fill columns would choke Explorer when scrolling
            // a folder of NEFs; pixel stats / HFR are skipped for RAW in v1.
            auto m = fitsx::parse_raw_metadata(buf_.data(), buf_.size());
            if (!m.success) return;
            img.width = m.width;
            img.height = m.height;
            img.headers = std::move(m.headers);
            break;
        }
        case fitsx::ImageFormat::Fits: {
            // Always emit metadata header-only, so columns show for EVERY FITS
            // -- including frames whose pixels extend past the 32 MB buffer cap
            // (a 26 Mpx 16-bit sub is ~52 MB, the common modern-CMOS case).
            // Reading pixels used to fail on those and drop ALL columns.
            auto md = fitsx::parse_fits_metadata(buf_.data(), buf_.size());
            if (!md.success) return;
            img.width   = md.width;
            img.height  = md.height;
            img.headers = std::move(md.headers);

            // If the whole file fits in the buffer (<= 32 MB), decode pixels too
            // and compute HFR/stars/stats in place, so the "sort by HFR in
            // Explorer" workflow keeps working instantly on the common case.
            // For > 32 MB frames the pixels aren't in the buffer, so analysis
            // defers to the cache the standalone viewer populates on first open.
            if (buf_.size() == buf_.total_size()) {
                auto loaded = fitsx::load_from_memory(buf_.data(), buf_.size());
                if (loaded.success) {
                    img = std::move(loaded.image);   // full image (pixels + headers)
                    can_run_analysis = true;
                }
                // On the rare failure with a full buffer, keep the header-only
                // metadata already emitted above.
            }
            break;
        }
        }

        // System.Image.HorizontalSize / VerticalSize  (UI4)
        if (img.width > 0)  add_uint(props_, PKEY_Image_HorizontalSize, static_cast<ULONG>(img.width));
        if (img.height > 0) add_uint(props_, PKEY_Image_VerticalSize,   static_cast<ULONG>(img.height));

        // System.Image.Dimensions (string  "WxH")
        {
            wchar_t buf[32] = {};
            swprintf_s(buf, L"%d x %d", img.width, img.height);
            add_string(props_, PKEY_Image_Dimensions, buf);
        }

        // OBJECT
        std::string s;
        if (img.find_header("OBJECT", s) && !s.empty()) {
            const auto w = utf8_to_wide(s);
            add_string(props_, PKEY_AstroFits_Object, w);
            add_string(props_, PKEY_Title, w);
        }

        // EXPTIME / EXPOSURE
        double d = 0.0;
        if (img.find_header_double("EXPTIME", d) || img.find_header_double("EXPOSURE", d)) {
            add_double(props_, PKEY_AstroFits_ExposureTime, round_n(d, 2));
        }

        // FILTER
        if (img.find_header("FILTER", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_Filter, utf8_to_wide(s));
        }

        // DATE-OBS
        if (img.find_header("DATE-OBS", s) && !s.empty()) {
            if (auto ft = fitsx::parse_dateobs_to_filetime(s); ft.has_value()) {
                add_filetime(props_, PKEY_AstroFits_DateObs, *ft);
                add_filetime(props_, PKEY_Photo_DateTaken, *ft);
            }
        }

        // GAIN
        long lv = 0;
        if (img.find_header_int("GAIN", lv)) {
            add_uint(props_, PKEY_AstroFits_Gain, static_cast<ULONG>(lv < 0 ? 0 : lv));
        }

        // CCD-TEMP / CCDTEMP
        if (img.find_header_double("CCD-TEMP", d) || img.find_header_double("CCDTEMP", d)) {
            add_double(props_, PKEY_AstroFits_CCDTemp, round_n(d, 1));
        }

        // TELESCOP
        if (img.find_header("TELESCOP", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_Telescope, utf8_to_wide(s));
        }

        // INSTRUME
        if (img.find_header("INSTRUME", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_Instrument, utf8_to_wide(s));
        }

        // OFFSET
        if (img.find_header_int("OFFSET", lv)) {
            add_uint(props_, PKEY_AstroFits_Offset, static_cast<ULONG>(lv < 0 ? 0 : lv));
        }

        // BAYERPAT
        if (img.find_header("BAYERPAT", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_BayerPattern, utf8_to_wide(s));
        }

        // FOCALLEN (mm)
        if (img.find_header_double("FOCALLEN", d)) {
            add_double(props_, PKEY_AstroFits_FocalLength, round_n(d, 0));
        }

        // FOCRATIO
        if (img.find_header_double("FOCRATIO", d)) {
            add_double(props_, PKEY_AstroFits_FocalRatio, round_n(d, 1));
        }

        // FOCPOS / FOCUSPOS (whichever the producer wrote)
        if (img.find_header_int("FOCPOS", lv) || img.find_header_int("FOCUSPOS", lv)) {
            add_uint(props_, PKEY_AstroFits_FocuserPosition, static_cast<ULONG>(lv < 0 ? 0 : lv));
        }

        // IMAGETYP
        if (img.find_header("IMAGETYP", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_ImageType, utf8_to_wide(s));
        }

        // SWCREATE
        if (img.find_header("SWCREATE", s) && !s.empty()) {
            add_string(props_, PKEY_AstroFits_Software, utf8_to_wide(s));
        }

        // -- Computed analysis (cached by content hash) ---------------------
        // FITS: compute on miss + store. XISF: lookup only (don't compute
        // synchronously - the standalone viewer populates the cache when the
        // user opens an XISF, after which Explorer columns light up).
        // Use buf_.total_size() so the key is identical whether the buffer
        // was fully or partially loaded (Property Handler has 32 MB cap;
        // the viewer reads everything).
        const std::string ckey = fitsx::compute_cache_key(buf_.data(), buf_.total_size());
        fitsx::AnalysisResult ar;
        bool have = false;
        if (auto cached = fitsx::AnalysisCache::instance().lookup(ckey); cached.has_value()) {
            ar = *cached;
            have = true;
        } else if (can_run_analysis) {
            ar = fitsx::run_analysis(img);
            fitsx::AnalysisCache::instance().store(ckey, ar);
            have = true;
        } else {
            // XISF cache miss: leave columns empty until the viewer fills the cache.
            return;
        }
        if (have && ar.success) {
            // Pixel statistics — always available even if star detection found nothing.
            // ADU-style values: integer display.
            add_double(props_, PKEY_AstroFits_StatMean,   round_n(ar.mean,   0));
            add_double(props_, PKEY_AstroFits_StatStdDev, round_n(ar.stddev, 0));
            add_double(props_, PKEY_AstroFits_StatMedian, round_n(ar.median, 0));
            add_double(props_, PKEY_AstroFits_StatMAD,    round_n(ar.mad,    0));

            wchar_t minBuf[64] = {};
            wchar_t maxBuf[64] = {};
            swprintf_s(minBuf, L"%g (%llux)",
                       ar.min_value, static_cast<unsigned long long>(ar.min_count));
            swprintf_s(maxBuf, L"%g (%llux)",
                       ar.max_value, static_cast<unsigned long long>(ar.max_count));
            add_string(props_, PKEY_AstroFits_StatMin, minBuf);
            add_string(props_, PKEY_AstroFits_StatMax, maxBuf);

            if (ar.star_count > 0) {
                add_uint  (props_, PKEY_AstroFits_AnalysisStarCount,    static_cast<ULONG>(ar.star_count));
                add_double(props_, PKEY_AstroFits_AnalysisHFR,          round_n(ar.hfr_median,          2));
                add_double(props_, PKEY_AstroFits_AnalysisHFRStdDev,    round_n(ar.hfr_stddev,          2));
                add_double(props_, PKEY_AstroFits_AnalysisFWHM,         round_n(ar.fwhm_median,         2));
                add_double(props_, PKEY_AstroFits_AnalysisEccentricity, round_n(ar.eccentricity_median, 2));
            }
        }
    } catch (...) {
        // Swallow everything; partially-populated props_ is fine.
    }
}

IFACEMETHODIMP FitsPropertyHandler::GetCount(DWORD* count) {
    if (!count) return E_POINTER;
    *count = static_cast<DWORD>(props_.size());
    return S_OK;
}

IFACEMETHODIMP FitsPropertyHandler::GetAt(DWORD index, PROPERTYKEY* key) {
    if (!key) return E_POINTER;
    if (index >= props_.size()) return E_INVALIDARG;
    *key = props_[index].first;
    return S_OK;
}

IFACEMETHODIMP FitsPropertyHandler::GetValue(REFPROPERTYKEY key, PROPVARIANT* value) {
    if (!value) return E_POINTER;
    PropVariantInit(value);
    for (const auto& p : props_) {
        if (p.first == key) {
            return PropVariantCopy(value, &p.second);
        }
    }
    return S_OK;  // not found: leave as VT_EMPTY, return S_OK per IPropertyStore contract
}

IFACEMETHODIMP FitsPropertyHandler::SetValue(REFPROPERTYKEY, REFPROPVARIANT) {
    return STG_E_ACCESSDENIED;
}

IFACEMETHODIMP FitsPropertyHandler::Commit() {
    return S_OK;
}

IFACEMETHODIMP FitsPropertyHandler::IsPropertyWritable(REFPROPERTYKEY) {
    return S_FALSE;
}
