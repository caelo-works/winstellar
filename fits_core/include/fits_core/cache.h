#pragma once

#include "analysis.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace fitsx {

// SQLite-backed analysis result cache. Single process-wide instance, opened
// lazily on first use. Failure to open the DB (read-only fs, locked file...)
// is non-fatal: the cache silently degrades to a no-op pass-through.
class AnalysisCache {
public:
    static AnalysisCache& instance();

    std::optional<AnalysisResult> lookup(std::string_view key);
    void store(std::string_view key, const AnalysisResult& r);

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
    bool open_attempted_ = false;
};

}  // namespace fitsx
