#pragma once

#include "analysis.h"
#include "fits_image.h"   // FitsHeader

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace fitsx {

// Just the header metadata a property handler needs to fill its columns without
// re-reading + re-parsing the file. Cached by content key so a folder revisit
// (Explorer re-queries every item) serves these from SQLite instead of reading
// megabytes per file again.
struct CachedMetadata {
    int width = 0;
    int height = 0;
    std::vector<FitsHeader> headers;
};

// SQLite-backed analysis result cache. Single process-wide instance, opened
// lazily on first use. Failure to open the DB (read-only fs, locked file...)
// is non-fatal: the cache silently degrades to a no-op pass-through.
class AnalysisCache {
public:
    static AnalysisCache& instance();

    std::optional<AnalysisResult> lookup(std::string_view key);
    void store(std::string_view key, const AnalysisResult& r);

    // Header-metadata cache (property-handler columns). Separate table/schema
    // from the analysis results.
    std::optional<CachedMetadata> lookup_metadata(std::string_view key);
    void store_metadata(std::string_view key, const CachedMetadata& m);

    AnalysisCache(const AnalysisCache&) = delete;
    AnalysisCache& operator=(const AnalysisCache&) = delete;

private:
    AnalysisCache() = default;
    ~AnalysisCache();

    void ensure_open();

    std::mutex m_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_lookup_ = nullptr;
    sqlite3_stmt* stmt_upsert_ = nullptr;
    sqlite3_stmt* stmt_meta_lookup_ = nullptr;
    sqlite3_stmt* stmt_meta_upsert_ = nullptr;
    bool open_attempted_ = false;
};

}  // namespace fitsx
