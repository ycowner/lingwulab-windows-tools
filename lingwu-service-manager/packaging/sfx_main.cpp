// sfx_main.cpp —— 单文件自解压启动器（self-extracting launcher）。
//
// 用途：把 dist\Release\ 整棵目录树（250 个文件 / 60.1 MB）嵌进**一个 exe**，
// 双击后自动释放到 %LOCALAPPDATA% 并启动真程序。真程序启动后本壳立即退出，
// 它自己不常驻 —— 只是个壳。
//
// 为什么不用现成方案：
//   · IExpress（系统自带）会**压平**目录结构。本工程实测压平必崩：
//     缺 Microsoft.UI.Xaml\Assets\NoiseAsset_256x256_PNG.png 时，
//     进程停在 MainWindow ctor 的 step=0 再退出（亚克力/Mica 背景噪点图
//     在运行期按固定相对路径加载，找不到就崩）。src\ui\*.xbf 同理。
//     ⇒ 目录结构是硬约束，必须自己保留。
//   · 7-Zip SFX / WinRAR 本机没装。
//
// 负载格式（make-sfx.ps1 生成，版本 1）：
//   magic  "LSFX"            4 字节
//   version                  uint32
//   buildIdLen               uint32      （UTF-16 字节数）
//   buildId                  UTF-16      例 "0.0.1-a1b2c3d4"
//   fileCount                uint32
//   重复 fileCount 次：
//     pathLen                uint32      （UTF-16 字节数）
//     path                   UTF-16      相对路径，'\' 分隔
//     size                   uint64      （原始字节数，不压缩）
//     bytes                  size 字节
//
// 释放目录：%LOCALAPPDATA%\LingwuServiceManager\portable\<buildId>\
//   buildId 进了目录名，所以同一份 exe 重复双击只会第一次真正解压；
//   换版本自然落到新目录，不会出现"新 exe 跑旧文件"的错觉。
//   释放完成后写 .sfx-ok 标记，标记已存在就整段跳过解压。
//
// 全部走 Win32 + 静态 CRT，运行时零依赖。
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// 小端 uint32 读出的 'L','S','F','X' 四个字节 = 0x5846534C。
// 别按字符串的直觉写成 0x5853464C（那是 "XFSL"），反了。
constexpr UINT32 kPayloadMagic = 0x5846534Cu;
constexpr UINT32 kPayloadVersion = 1;
constexpr int kPayloadResId = 1;

// 资源类型必须按**整数 ID** 查，不能按字符串名查。
// rc.exe 遇到 `1 RCDATA "payload.bin"` 会把它存成类型 ID 10（RT_RCDATA）；
// 传 L"RCDATA" 查的是「名为 RCDATA 的自定义类型」，那个条目不存在，
// FindResourceW 直接返回 NULL —— 症状极具迷惑性：
// 「负载明明在 exe 里（全文搜得到 LSFX 魔数）却报找不到内嵌负载」。
constexpr int kPayloadResType = 10;  // == RT_RCDATA

// MAKEINTRESOURCE 的等价展开。SDK 的 MAKEINTRESOURCEW 宏在当前包含链下
// 拿不到（C3861），而 MAKEINTRESOURCE 展开成 ANSI 的 LPSTR、喂给
// FindResourceW 又 C2664。手写展开最省事，也最清楚。
inline LPWSTR ResId(int id) {
    return reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(static_cast<WORD>(id)));
}

std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(buf, n);
}

// 逐级建目录。CreateDirectoryW 只能建一级，所以沿 '\' 一层层建。
void EnsureDirTree(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring sub = dir.substr(0, i);
            ::CreateDirectoryW(sub.c_str(), nullptr);  // 已存在时返回失败，无所谓
        }
    }
}

// 负载里的路径必须留在释放目录内。防的是"条目里带 ..\ 把文件写到别处"。
// 负载虽然是自己生成的，但这是从磁盘读进内存再照着写盘的路径，
// 值得把这条防线留着 —— 以后有人手改 payload.bin 也不会变成任意写。
bool IsSafeRelativePath(const std::wstring& rel) {
    if (rel.empty() || rel.size() >= 32760) return false;
    if (rel[0] == L'\\' || rel[0] == L'/') return false;
    if (rel.find(L':') != std::wstring::npos) return false;   // 挡 "C:\" 和 ADS
    if (rel.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : rel) {
        if (c < 0x20 || c == L'|') return false;
    }
    return true;
}

// 顺序读。越界一律置失败位而不是抛异常 —— 这段跑在 wWinMain 里，
// 没有能承接异常的地方，崩掉的话连错误信息都没有。
class Reader {
public:
    Reader(const BYTE* data, size_t size) : p_(data), end_(data + size) {}

    bool ok() const { return ok_; }

    UINT32 u32() {
        if (end_ - p_ < 4) { ok_ = false; return 0; }
        UINT32 v = 0;
        std::memcpy(&v, p_, 4);
        p_ += 4;
        return v;
    }
    UINT64 u64() {
        if (end_ - p_ < 8) { ok_ = false; return 0; }
        UINT64 v = 0;
        std::memcpy(&v, p_, 8);
        p_ += 8;
        return v;
    }
    std::wstring wstr() {
        UINT32 bytes = u32();
        if (!ok_ || static_cast<size_t>(end_ - p_) < bytes || (bytes % 2) != 0) {
            ok_ = false;
            return {};
        }
        std::wstring s(reinterpret_cast<const wchar_t*>(p_), bytes / 2);
        p_ += bytes;
        return s;
    }
    // 取一段原始字节的指针并前移。调用方必须**同步**用完这段内存
    // （本文件里紧跟着 WriteFile，没有跨函数持有，安全）。
    const BYTE* Take(UINT64 n) {
        if (!ok_ || static_cast<UINT64>(end_ - p_) < n) { ok_ = false; return nullptr; }
        const BYTE* r = p_;
        p_ += n;
        return r;
    }

private:
    const BYTE* p_;
    const BYTE* end_;
    bool ok_ = true;
};

void Fail(const std::wstring& what) {
    ::MessageBoxW(nullptr, what.c_str(), L"Lingwu Service Manager", MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HRSRC hRes = ::FindResourceW(hInst, ResId(kPayloadResId), ResId(kPayloadResType));
    if (!hRes) {
        Fail(L"启动器损坏：找不到内嵌负载（RCDATA #1）。");
        return 2;
    }
    HGLOBAL hGlob = ::LoadResource(hInst, hRes);
    if (!hGlob) {
        Fail(L"启动器损坏：负载读取失败。");
        return 2;
    }
    const BYTE* base = static_cast<const BYTE*>(::LockResource(hGlob));
    const size_t total = ::SizeofResource(hInst, hRes);
    if (!base || total < 20) {
        Fail(L"启动器损坏：负载为空。");
        return 2;
    }

    Reader rd(base, total);
    if (rd.u32() != kPayloadMagic || rd.u32() != kPayloadVersion) {
        Fail(L"启动器损坏：负载格式不匹配（需要 LSFX v1）。");
        return 2;
    }
    const std::wstring buildId = rd.wstr();
    const UINT32 fileCount = rd.u32();
    if (!rd.ok() || buildId.empty() || fileCount == 0 || fileCount > 100000) {
        Fail(L"启动器损坏：负载头部无效。");
        return 2;
    }

    const std::wstring base_dir = AppDataDir();
    if (base_dir.empty()) {
        Fail(L"读取 LOCALAPPDATA 失败，无法确定释放位置。");
        return 3;
    }
    const std::wstring dest = base_dir + L"\\LingwuServiceManager\\portable\\" + buildId;
    const std::wstring marker = dest + L"\\.sfx-ok";

    // 同一份 exe 再双击：标记在就直接启动，不重复解 60 MB。
    if (::GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) {
        EnsureDirTree(dest);

        for (UINT32 i = 0; i < fileCount && rd.ok(); ++i) {
            const std::wstring rel = rd.wstr();
            const UINT64 size = rd.u64();
            if (!rd.ok() || !IsSafeRelativePath(rel)) {
                Fail(L"启动器损坏：负载条目无效。");
                return 2;
            }
            const BYTE* src = rd.Take(size);
            if (!rd.ok() || !src) {
                Fail(L"启动器损坏：负载提前结束。");
                return 2;
            }

            const std::wstring full = dest + L"\\" + rel;
            const size_t slash = full.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                EnsureDirTree(full.substr(0, slash));
            }

            HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                Fail(L"写入失败：\n" + rel);
                return 4;
            }
            DWORD chunk = 0;
            UINT64 left = size;
            while (left > 0) {
                const DWORD want =
                    static_cast<DWORD>(left > 0x10000000ull ? 0x10000000ull : left);
                if (!::WriteFile(h, src, want, &chunk, nullptr) || chunk != want) {
                    ::CloseHandle(h);
                    Fail(L"写入中断：\n" + rel);
                    return 4;
                }
                src += want;
                left -= want;
            }
            ::CloseHandle(h);
        }

        if (!rd.ok()) {
            Fail(L"启动器损坏：负载提前结束。");
            return 2;
        }
        HANDLE h = ::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }

    const std::wstring target = dest + L"\\LingwuServiceManager.exe";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = target.c_str();
    sei.lpDirectory = dest.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&sei)) {
        Fail(L"启动失败：\n" + target);
        return 5;
    }
    return 0;   // 壳立刻退出，真程序独立运行
}
