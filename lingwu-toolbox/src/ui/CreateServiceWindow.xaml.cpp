#include "pch.h"
#include "CreateServiceWindow.xaml.h"
// XAML 生成代码必须在这里显式引入一次（官方模板也是这么做的）。
#if __has_include("CreateServiceWindow.g.cpp")
#include "CreateServiceWindow.g.cpp"
#endif
#if __has_include("CreateServiceWindow.xaml.g.hpp")
#include "CreateServiceWindow.xaml.g.hpp"
#endif
#include <microsoft.ui.xaml.window.h>

#include <cwctype>

#include "platform/trace.h"
#include "platform/window_decor.h"

// shobjidl 的 FileOpenDialog。用 CoCreateInstance 而不引 Windows.Storage
// 的 FilePicker：后者在桌面应用里要先挂 IFileOpenDialog 的事件才能拿到结果，
// 而 IFileOpenDialog 直接 Show() 返回 HRESULT 就够了，代码短得多也更稳。
#include <shobjidl.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Dispatching;
using namespace winrt::Windows::Foundation;

namespace winrt::LingwuToolbox::implementation {
namespace core = lingwu::core::service;

namespace {

constexpr size_t kMaxServiceNameLen = 256;

bool FileExists(std::wstring const& path) {
    if (path.empty()) return false;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    // FILE_ATTRIBUTE_INVALID 是唯一能确定「不存在」的返回值；
    // 目录存在也算（服务可以指向一个目录？不行，所以只当 exe 用，
    // 但我们不因此报错 —— 路径可能指向网络盘或尚未生成的文件）。
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

CreateServiceWindow::CreateServiceWindow() {
    LINGWU_TRACE("CreateServiceWindow ctor: enter");
    try {
        InitializeComponent();
    } catch (winrt::hresult_error const& err) {
        LINGWU_TRACE("CreateServiceWindow InitializeComponent threw hresult=0x%08X",
                      static_cast<unsigned>(err.code().value));
        throw;
    }

    nameError_ = ServiceNameError();
    displayError_ = DisplayNameError();
    pathError_ = BinaryPathError();
    pathWarning_ = BinaryPathWarning();

    // 启动类型四档，顺序 = StartTypeChoice 的枚举值。
    for (int i = 0; i <= 3; ++i) {
        auto item = ComboBoxItem();
        item.Content(box_value(
            core::StartTypeChoiceText(static_cast<core::StartTypeChoice>(i))));
        StartTypeBox().Items().Append(item);
    }
    StartTypeBox().SelectedIndex(0);   // 默认「自动」

    // 三个内置账户。显示名给中文，写进去的是 SCM 认识的英文。
    {
        // 按值取，不要 auto&：Items() 是**返回值**，不是引用成员，
        // 绑到 auto& 上会 C2440（拿临时量初始化非 const 引用）。
        auto items = AccountBox().Items();
        for (const wchar_t* acct : kAccounts) {
            auto item = ComboBoxItem();
            wchar_t label[128];
            ::swprintf_s(label, L"%ls（%ls）",
                         acct,
                         (::wcscmp(acct, L"LocalSystem") == 0)     ? L"本地系统"
                         : (::wcscmp(acct, L"LocalService") == 0)   ? L"本地服务"
                                                                     : L"网络服务");
            item.Content(box_value(label));
            items.Append(item);
        }
        AccountBox().SelectedIndex(0);
    }

    BrowseButton().Click([this](auto&&, auto&&) {
        const hstring picked = PickBinaryFile();
        if (picked.empty()) {
            return;   // 用户取消 —— 不能当成「路径变成空了」
        }
        BinaryPathBox().Text(picked);
    });

    // 【这两个按钮的处理器曾经完全没挂】
    // 构造函数里只接了「浏览…」和心跳，创建/取消两个按钮没有任何
    // Click 处理器。后果：点「创建」毫无反应（窗口不关、主窗口也收
    // 不到任何字段），点「取消」同样毫无反应，只能用右上角 X 关窗。
    //
    // 它能一路混过编译、--selftest 甚至大部分 UI 验收，因为「按钮
    // 能不能点」和「点击后有没有接线」是两件完全不同的事，
    // 只有真的 InvokePattern.Invoke() 一次才看得出来。
    //
    // 「创建」只负责关窗口：字段回读与提交都在主窗口的 Closed 回调里
    // （见 MainWindow.xaml.cpp 的 OnNewServiceClicked）。这样「用户到底
    // 填了什么」永远以关闭那一刻为准，不存在两份可能不一致的副本。
    CreateButton().Click([this](auto&&, auto&&) {
        accepted_ = true;
        LINGWU_TRACE("CreateButton clicked; closing window");
        Close();
    });
    CancelButton().Click([this](auto&&, auto&&) {
        LINGWU_TRACE("CancelButton clicked; closing window");
        Close();
    });

    // TextChanged 属 Microsoft.UI.*，本版 cppwinrt 接不上 ——
    // 与主窗口处理两个 ComboBox 同一套路，用心跳比对文本内容。
    // 100 ms 的延迟用户完全感知不到，而换来的是零委托依赖。
    // 同 MainWindow：WinUI 3 的 DispatcherQueueTimer 没有默认构造函数。
    timer_ = this->DispatcherQueue().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(100));
    timer_.Tick([this](auto&&, auto&&) { ValidateNow(); });
    timer_.Start();

    // 每次打开都从默认值开始，并重跑一次校验把错误提示清干净。
    // 常驻复用窗口时不清的话，上一次留下的红字会跟着进来，
    // 表现为「刚打开就有个不存在的错误」。
    //
    // accepted_ 也必须在这里复位：它跨窗口生命周期存活，不复位的话
    // 上一次点过「创建」，这次随便开一下再按 X，服务照样会被建出来。
    accepted_ = false;
    ServiceNameBox().Text(L"");
    DisplayNameBox().Text(L"");
    BinaryPathBox().Text(L"");
    DescriptionBox().Text(L"");
    StartImmediatelyBox().IsChecked(false);
    lastServiceName_ = L"";
    lastDisplayName_ = L"";
    lastBinaryPath_ = L"";
    ValidateNow();

    try {
        constexpr double kDefaultW = 0.42;
        constexpr double kDefaultH = 0.62;
        const auto work = DisplayArea::GetFromWindowId(
            AppWindow().Id(), DisplayAreaFallback::Nearest).WorkArea();
        const int w = static_cast<int>(static_cast<double>(work.Width) * kDefaultW);
        const int h = static_cast<int>(static_cast<double>(work.Height) * kDefaultH);
        const int x = work.X + (work.Width - w) / 2;
        const int y = work.Y + (work.Height - h) / 2;
        AppWindow().MoveAndResize(winrt::Windows::Graphics::RectInt32{ x, y, w, h });
    } catch (winrt::hresult_error const&) {
        AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 620, 720 });
    }
    if (auto pres = AppWindow().Presenter().try_as<OverlappedPresenter>()) {
        pres.PreferredMinimumWidth(560);
        pres.PreferredMinimumHeight(520);
    }

    HWND hwnd{};
    if (SUCCEEDED(this->m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd))) {
        platform::ApplyWindowDecorations(hwnd);
    }
    LINGWU_TRACE("CreateServiceWindow ctor: done");
}

void CreateServiceWindow::ValidateNow() {
    const std::wstring name{ ServiceNameBox().Text() };
    const std::wstring display{ DisplayNameBox().Text() };
    const std::wstring path{ BinaryPathBox().Text() };

    if (name == lastServiceName_ && display == lastDisplayName_ &&
        path == lastBinaryPath_) {
        return;
    }
    lastServiceName_ = name;
    lastDisplayName_ = display;
    lastBinaryPath_ = path;

    const hstring nameErr = ValidateName(hstring{ name });
    const hstring displayErr = ValidateDisplayName(hstring{ display });
    const hstring pathErr = ValidateBinaryPath(hstring{ path });

    nameError_.Text(nameErr);
    displayError_.Text(displayErr);
    pathError_.Text(pathErr);

    // 文件不存在是**警告不是错误**：服务路径可能指向网络位置，
    // 或者可执行文件本来就要等安装程序结束时才生成。
    // 挡住不让创建会让用户先建个空服务再来补路径，多此一举。
    if (pathErr.empty() && !path.empty() && !FileExists(path)) {
        // Text() 收 hstring const&，不能喂 box_value —— 那返回 IInspectable，
        // 会报 C2665「没有可转换的重载」。
        pathWarning_.Text(hstring(
            L"当前路径下找不到这个文件。服务仍可创建，但启动时会失败。"));
        pathWarning_.Visibility(Visibility::Visible);
    } else {
        pathWarning_.Visibility(Visibility::Collapsed);
    }

    // 校验不过就禁用创建按钮，**并且就地说明原因**。
    // 点了才弹框的校验等于没有 —— 用户只能反复试。
    CreateButton().IsEnabled(nameErr.empty() && displayErr.empty() && pathErr.empty());
}

winrt::hstring CreateServiceWindow::ValidateName(winrt::hstring const& serviceName) {
    const std::wstring s{ serviceName };
    if (s.empty()) {
        return hstring{ L"服务名不能为空。" };
    }
    if (s.size() > kMaxServiceNameLen) {
        return hstring{ L"服务名太长（上限 256 个字符）。" };
    }
    // 反斜杠与正斜杠在 SCM 里有特殊含义，传进去会被当成路径。
    if (s.find(L'\\') != std::wstring::npos || s.find(L'/') != std::wstring::npos) {
        return hstring{ L"服务名不能包含反斜杠或正斜杠。" };
    }
    if (s.front() == L'.' || s.back() == L'.') {
        return hstring{ L"服务名不能以点开头或结尾。" };
    }
    for (wchar_t c : s) {
        if (c < 0x20) {
            return hstring{ L"服务名不能包含控制字符。" };
        }
    }
    return hstring{ L"" };
}

winrt::hstring CreateServiceWindow::ValidateDisplayName(winrt::hstring const& displayName) {
    const std::wstring s{ displayName };
    if (s.empty()) {
        return hstring{ L"显示名不能为空。" };
    }
    if (s.size() > 256) {
        return hstring{ L"显示名太长（上限 256 个字符）。" };
    }
    return hstring{ L"" };
}

winrt::hstring CreateServiceWindow::ValidateBinaryPath(winrt::hstring const& binaryPath) {
    const std::wstring s{ binaryPath };
    if (s.empty()) {
        return hstring{ L"必须填写可执行文件路径。" };
    }
    if (s.size() > 32767) {
        return hstring{ L"路径太长。" };
    }
    // 引号自己配，不让用户管（见 scm.cpp 的 QuoteBinaryPath）。
    // 这里拒绝用户手写的成对引号之外的畸形引号，
    // 否则 CreateServiceW 会把整串当成一个不存在的文件名。
    const size_t quotes = std::count(s.begin(), s.end(), L'"');
    if (quotes != 0 && quotes != 2) {
        return hstring{ L"路径中的引号不配对，请去掉。" };
    }
    if (quotes == 2 && (s.front() != L'"' || s.back() != L'"')) {
        return hstring{ L"成对的引号必须包住整条路径。" };
    }
    return hstring{ L"" };
}

winrt::hstring CreateServiceWindow::PickBinaryFile() {
    // IFileOpenDialog 只能在 STA 用；本工程 init_apartment(single_threaded)
    // 已经保证了，这里不用再查。
    winrt::com_ptr<IFileOpenDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr)) {
        LINGWU_TRACE("CoCreateInstance(FileOpenDialog) failed 0x%08X",
                      static_cast<unsigned>(hr));
        return hstring{ L"" };
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    // 过滤器：可执行文件 + 全部文件。
    COMDLG_FILTERSPEC filters[2] = {
        { L"可执行文件 (*.exe)", L"*.exe" },
        { L"全部文件 (*.*)", L"*.*" },
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);

    // 从当前已填的路径起步，浏览体验好很多。
    const std::wstring cur{ BinaryPathBox().Text() };
    if (!cur.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr,
                                                  IID_PPV_ARGS(&item))) && item) {
            dialog->SetFolder(item);
            item->Release();
        }
    }

    hr = dialog->Show(nullptr);
    if (FAILED(hr)) {
        // ERROR_CANCELLED（0x800704C7）是用户取消，不是错误 —— 安静返回空串。
        LINGWU_TRACE("FileOpenDialog::Show hr=0x%08X", static_cast<unsigned>(hr));
        return hstring{ L"" };
    }

    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) {
        return hstring{ L"" };
    }
    // GetDisplayName 的第二参是 LPWSTR*，而且它 CoTaskMemAlloc 一块
    // 自己长度的内存 —— 既不能塞进 wchar_t[MAX_PATH]，也不能写 &buf
    // （那是 wchar_t(*)[N]，报 C2664）。按 COM 约定分配/释放才对。
    LPWSTR allocated = nullptr;
    const HRESULT got = item->GetDisplayName(SIGDN_FILESYSPATH, &allocated);
    item->Release();
    if (FAILED(got) || !allocated) {
        return hstring(L"");
    }
    // 补引号是 scm.cpp 的职责，这里回填**裸路径**给用户看。
    const hstring result(allocated);
    ::CoTaskMemFree(allocated);
    return result;
}

winrt::hstring CreateServiceWindow::ServiceNameInput() {
    return ServiceNameBox().Text();
}

winrt::hstring CreateServiceWindow::DisplayNameInput() {
    return DisplayNameBox().Text();
}

winrt::hstring CreateServiceWindow::DescriptionInput() {
    return DescriptionBox().Text();
}

winrt::hstring CreateServiceWindow::BinaryPathInput() {
    return BinaryPathBox().Text();
}

int32_t CreateServiceWindow::StartTypeChoice() {
    // ComboBox 一个项都没选中时 SelectedIndex 是 -1，clamp 回 0（自动）。
    // 绝不能把 -1 透给 MainWindow：StartTypeToDWord 拿它去查表。
    const int st = StartTypeBox().SelectedIndex();
    return (st < 0 || st > 3) ? 0 : st;
}

winrt::hstring CreateServiceWindow::AccountInput() {
    // 同上，索引越界一律退回 LocalSystem。
    const int acc = AccountBox().SelectedIndex();
    return winrt::hstring((acc < 0 || acc > 2) ? kAccounts[0] : kAccounts[acc]);
}

bool CreateServiceWindow::StartImmediatelyInput() {
    // 千万别把类型写成 IPropertyValue：它只有 GetBoolean()，没有 Value()。
    // IsChecked() 的投影类型是 IReference<bool>，成员才叫 Value()。
    const auto checked = StartImmediatelyBox().IsChecked();
    return (checked && checked.Value());
}

bool CreateServiceWindow::Accepted() {
    return accepted_;
}

}  // namespace winrt::LingwuToolbox::implementation
