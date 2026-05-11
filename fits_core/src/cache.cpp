#include "fits_core/cache.h"

#include <windows.h>
#include <shlobj.h>

#include <sqlite3.h>

#include <cstdlib>
#include <string>

namespace fitsx {

namespace {

constexpr const char* kCreateSql =
    "CREATE TABLE IF NOT EXISTS analysis ("
    "  cache_key TEXT PRIMARY KEY,"
    "  schema_version INTEGER NOT NULL,"
    "  computed_at INTEGER NOT NULL,"
    "  success INTEGER NOT NULL,"
    "  star_count INTEGER,"
    "  hfr_median REAL,"
    "  hfr_stddev REAL,"
    "  fwhm_median REAL,"
    "  ecc_median REAL,"
    "  pix_mean REAL,"
    "  pix_stddev REAL,"
    "  pix_median REAL,"
    "  pix_mad REAL,"
    "  pix_min REAL,"
    "  pix_min_count INTEGER,"
    "  pix_max REAL,"
    "  pix_max_count INTEGER"
    ");";

constexpr const char* kLookupSql =
    "SELECT success, star_count, hfr_median, hfr_stddev, fwhm_median, ecc_median,"
    "       pix_mean, pix_stddev, pix_median, pix_mad, pix_min, pix_min_count,"
    "       pix_max, pix_max_count "
    "FROM analysis "
    "WHERE cache_key = ?1 AND schema_version = ?2;";

constexpr const char* kUpsertSql =
    "INSERT OR REPLACE INTO analysis (cache_key, schema_version, computed_at, success,"
    " star_count, hfr_median, hfr_stddev, fwhm_median, ecc_median,"
    " pix_mean, pix_stddev, pix_median, pix_mad, pix_min, pix_min_count, pix_max, pix_max_count)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17);";

std::wstring resolve_db_path() {
    const wchar_t* la = _wgetenv(L"LOCALAPPDATA");
    if (!la || !*la) return {};
    std::wstring dir = std::wstring(la) + L"\\WinStellar";
    ::CreateDirectoryW(dir.c_str(), nullptr);  // OK if it already exists
    return dir + L"\\analysis_cache.db";
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                   static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

AnalysisCache& AnalysisCache::instance() {
    static AnalysisCache inst;
    return inst;
}

AnalysisCache::~AnalysisCache() {
    if (stmt_lookup_) sqlite3_finalize(stmt_lookup_);
    if (stmt_upsert_) sqlite3_finalize(stmt_upsert_);
    if (db_) sqlite3_close(db_);
}

void AnalysisCache::ensure_open() {
    if (open_attempted_) return;
    open_attempted_ = true;

    const std::string path = wide_to_utf8(resolve_db_path());
    if (path.empty()) return;

    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    // WAL lets Explorer + SearchIndexer + the viewer read concurrently while
    // any one of them writes; keep busy timeout short.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout(db_, 250);

    if (sqlite3_exec(db_, kCreateSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db_); db_ = nullptr; return;
    }

    if (sqlite3_prepare_v2(db_, kLookupSql, -1, &stmt_lookup_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, kUpsertSql, -1, &stmt_upsert_, nullptr) != SQLITE_OK) {
        if (stmt_lookup_) { sqlite3_finalize(stmt_lookup_); stmt_lookup_ = nullptr; }
        if (stmt_upsert_) { sqlite3_finalize(stmt_upsert_); stmt_upsert_ = nullptr; }
        sqlite3_close(db_); db_ = nullptr;
        return;
    }
}

std::optional<AnalysisResult> AnalysisCache::lookup(std::string_view key) {
    std::lock_guard<std::mutex> lk(m_);
    ensure_open();
    if (!db_ || !stmt_lookup_) return std::nullopt;

    sqlite3_reset(stmt_lookup_);
    sqlite3_clear_bindings(stmt_lookup_);
    sqlite3_bind_text(stmt_lookup_, 1, key.data(), static_cast<int>(key.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_lookup_, 2, kAnalysisSchemaVersion);

    if (sqlite3_step(stmt_lookup_) != SQLITE_ROW) return std::nullopt;

    AnalysisResult r;
    int col = 0;
    r.success            = sqlite3_column_int(stmt_lookup_, col++) != 0;
    r.star_count         = sqlite3_column_int(stmt_lookup_, col++);
    r.hfr_median         = sqlite3_column_double(stmt_lookup_, col++);
    r.hfr_stddev         = sqlite3_column_double(stmt_lookup_, col++);
    r.fwhm_median        = sqlite3_column_double(stmt_lookup_, col++);
    r.eccentricity_median = sqlite3_column_double(stmt_lookup_, col++);
    r.mean               = sqlite3_column_double(stmt_lookup_, col++);
    r.stddev             = sqlite3_column_double(stmt_lookup_, col++);
    r.median             = sqlite3_column_double(stmt_lookup_, col++);
    r.mad                = sqlite3_column_double(stmt_lookup_, col++);
    r.min_value          = sqlite3_column_double(stmt_lookup_, col++);
    r.min_count          = static_cast<uint64_t>(sqlite3_column_int64(stmt_lookup_, col++));
    r.max_value          = sqlite3_column_double(stmt_lookup_, col++);
    r.max_count          = static_cast<uint64_t>(sqlite3_column_int64(stmt_lookup_, col++));
    return r;
}

void AnalysisCache::store(std::string_view key, const AnalysisResult& r) {
    std::lock_guard<std::mutex> lk(m_);
    ensure_open();
    if (!db_ || !stmt_upsert_) return;

    sqlite3_reset(stmt_upsert_);
    sqlite3_clear_bindings(stmt_upsert_);
    int i = 1;
    sqlite3_bind_text(stmt_upsert_, i++, key.data(), static_cast<int>(key.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_upsert_, i++, kAnalysisSchemaVersion);
    sqlite3_bind_int64(stmt_upsert_, i++, static_cast<sqlite3_int64>(time(nullptr)));
    sqlite3_bind_int(stmt_upsert_, i++, r.success ? 1 : 0);
    sqlite3_bind_int(stmt_upsert_, i++, r.star_count);
    sqlite3_bind_double(stmt_upsert_, i++, r.hfr_median);
    sqlite3_bind_double(stmt_upsert_, i++, r.hfr_stddev);
    sqlite3_bind_double(stmt_upsert_, i++, r.fwhm_median);
    sqlite3_bind_double(stmt_upsert_, i++, r.eccentricity_median);
    sqlite3_bind_double(stmt_upsert_, i++, r.mean);
    sqlite3_bind_double(stmt_upsert_, i++, r.stddev);
    sqlite3_bind_double(stmt_upsert_, i++, r.median);
    sqlite3_bind_double(stmt_upsert_, i++, r.mad);
    sqlite3_bind_double(stmt_upsert_, i++, r.min_value);
    sqlite3_bind_int64(stmt_upsert_, i++, static_cast<sqlite3_int64>(r.min_count));
    sqlite3_bind_double(stmt_upsert_, i++, r.max_value);
    sqlite3_bind_int64(stmt_upsert_, i++, static_cast<sqlite3_int64>(r.max_count));

    sqlite3_step(stmt_upsert_);
}

}  // namespace fitsx
