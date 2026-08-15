// End-to-end export through the real executable.
//
// This is the only automated coverage of the WIC lane: WIC needs COM, a real
// encoder and a real file, none of which a unit test can stand in for. The
// child is run for every format, because the most likely runtime failure of an
// export worker -- CO_E_NOTINITIALIZED on a thread with no COM -- is invisible
// until the code actually runs on a spawned thread, which --export does.

#include "helpers/synth_fits.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef WINSTELLAR_EXE_PATH
#  error "WINSTELLAR_EXE_PATH must be defined by CMake"
#endif

namespace {

// The exe path is baked in as a narrow literal; widen it once. Never build the
// range from two mentions of the macro -- without string pooling (off in Debug)
// each mention is a distinct object and the iterator range is transposed.
std::wstring viewer_exe_path() {
    const std::string narrow = WINSTELLAR_EXE_PATH;
    return std::wstring(narrow.begin(), narrow.end());
}

DWORD run(const std::wstring& args, DWORD timeout_ms = 60000) {
    std::wstring cmd = L"\"" + viewer_exe_path() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    if (!::CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ADD_FAILURE() << "CreateProcessW failed, GetLastError=" << ::GetLastError();
        return STILL_ACTIVE;
    }
    if (::WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 0xDEAD);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        ADD_FAILURE() << "export timed out after " << timeout_ms << " ms";
        return STILL_ACTIVE;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return code;
}

fs::path temp_dir() {
    wchar_t base[MAX_PATH];
    ::GetTempPathW(MAX_PATH, base);
    fs::path d = fs::path(base) / (L"winstellar_smoke_export_" +
                                   std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::vector<uint8_t> head_of(const fs::path& p, size_t n) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> v(n);
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
    v.resize(static_cast<size_t>(f.gcount()));
    return v;
}

bool starts_with(const std::vector<uint8_t>& v, std::initializer_list<uint8_t> magic) {
    if (v.size() < magic.size()) return false;
    size_t i = 0;
    for (uint8_t b : magic) if (v[i++] != b) return false;
    return true;
}

struct Case { const wchar_t* fmt; const wchar_t* ext; };

}  // namespace

class SmokeExport : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = temp_dir();
        src_ = dir_ / "in.fits";
        auto spec = wst::make_star_field(64, 48, /*n*/ 3, 1000.0f, 20.0f, 25000.0f, 1.4f);
        ASSERT_FALSE(wst::write_synth_fits(src_.string(), spec).empty());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_, src_;
};

TEST_F(SmokeExport, EveryFormatWritesAReadableFile) {
    const Case cases[] = {
        {L"jpg", L"jpg"}, {L"png", L"png"}, {L"tif", L"tif"}, {L"fits", L"fits"},
    };
    for (const auto& c : cases) {
        const fs::path out = dir_ / (std::wstring(L"out.") + c.ext);
        const std::wstring args = std::wstring(L"--export ") + c.fmt +
                                  L" \"" + src_.wstring() + L"\"" +
                                  L" \"" + out.wstring() + L"\"";
        EXPECT_EQ(run(args), 0u) << "format=" << c.fmt;
        ASSERT_TRUE(fs::exists(out)) << "format=" << c.fmt;
        EXPECT_GT(fs::file_size(out), 0u) << "format=" << c.fmt;

        const auto head = head_of(out, 16);
        const std::wstring f = c.fmt;
        if (f == L"jpg")       EXPECT_TRUE(starts_with(head, {0xFF, 0xD8, 0xFF}));
        else if (f == L"png")  EXPECT_TRUE(starts_with(head, {0x89, 'P', 'N', 'G'}));
        else if (f == L"tif")  EXPECT_TRUE(starts_with(head, {'I', 'I', 0x2A, 0x00}) ||
                                           starts_with(head, {'M', 'M', 0x00, 0x2A}));
        else                   EXPECT_TRUE(starts_with(head, {'S', 'I', 'M', 'P', 'L', 'E'}));
    }
}

TEST_F(SmokeExport, RotatedExportSwapsTheDimensions) {
    // Also the only automated check that the rotation survives the whole chain.
    const fs::path out = dir_ / "rot.png";
    const std::wstring args = L"--export png \"" + src_.wstring() + L"\" \"" +
                              out.wstring() + L"\" --rot 90";
    ASSERT_EQ(run(args), 0u);

    // PNG IHDR: width and height as big-endian u32 at offset 16.
    const auto head = head_of(out, 24);
    ASSERT_GE(head.size(), 24u);
    const uint32_t w = (head[16] << 24) | (head[17] << 16) | (head[18] << 8) | head[19];
    const uint32_t h = (head[20] << 24) | (head[21] << 16) | (head[22] << 8) | head[23];
    EXPECT_EQ(w, 48u) << "a 64x48 frame rotated 90 must come out 48x64";
    EXPECT_EQ(h, 64u);
}

TEST_F(SmokeExport, ExportedFitsReloadsWithOurOwnLoader) {
    const fs::path out = dir_ / "round.fits";
    ASSERT_EQ(run(L"--export fits \"" + src_.wstring() + L"\" \"" + out.wstring() + L"\""), 0u);
    // --check runs the full load + analysis pipeline over it.
    EXPECT_EQ(run(L"--check \"" + out.wstring() + L"\""), 0u)
        << "the exported FITS is not readable by the application that wrote it";
}

TEST_F(SmokeExport, BadArgumentsFailInsteadOfWritingSomething) {
    const fs::path out = dir_ / "never.jpg";
    EXPECT_NE(run(L"--export bmp \"" + src_.wstring() + L"\" \"" + out.wstring() + L"\""), 0u);
    EXPECT_FALSE(fs::exists(out));

    const fs::path out2 = dir_ / "never2.jpg";
    EXPECT_NE(run(L"--export jpg \"" + (dir_ / "missing.fits").wstring() + L"\" \"" +
                  out2.wstring() + L"\""), 0u);
    EXPECT_FALSE(fs::exists(out2));

    EXPECT_NE(run(L"--export jpg"), 0u);
}

TEST_F(SmokeExport, NoTemporaryFilesSurvive) {
    const fs::path out = dir_ / "clean.tif";
    ASSERT_EQ(run(L"--export tif \"" + src_.wstring() + L"\" \"" + out.wstring() + L"\""), 0u);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        const std::wstring n = e.path().filename().wstring();
        EXPECT_EQ(n.find(L".winstellar-tmp-"), std::wstring::npos)
            << "left behind: " << e.path().string();
    }
}
