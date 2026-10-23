#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 MainWindow.g.h。
//
// 它内部有
//     #if __has_include("ServiceMainWindow.xaml.g.h")
//         #include "ServiceServiceMainWindow.xaml.g.h"
//     #else
//         ... ServiceMainWindowT = ServiceMainWindow_base   // 退化成没有 XAML 代码生成的纯壳
//     #endif
// 也就是说 XAML 代码后置头是被自动带进来的；一旦 __has_include 落空，
// 编译能过但 InitializeComponent() 之类全都不存在，症状是莫名其妙的编译错误。
// vcxproj 里为此把 $(IntDir)src\ui 加进了 AdditionalIncludeDirectories。
#include "ServiceMainWindow.g.h"

#include "pch.h"

#include "core/service/controller.h"
#include "core/settings.h"
#include "core/service/service_model.h"

// 新建服务窗口。主窗口持有它的强引用并接收 Closed 回调，
// 所以这里要看到完整定义而不是前置声明。
#include "CreateServiceWindow.xaml.h"

// ServiceRow 必须在这里可见：x:DataType="local:ServiceRow" 让 XAML 编译器生成的
// x:Bind 代码在 ServiceMainWindow.xaml.g.cpp 里直接访问它的成员，而那个 .g.cpp 是被
// MainWindow.xaml.cpp 在本头之后 include 的。
#include "ServiceRow.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// single_threaded_observable_vector 只在这个头里声明，pch.h 的 WinRT 投影列表没有它。
#include <winrt/Windows.Foundation.Collections.h>

namespace winrt::LingwuToolbox::implementation {

// 窗口子类与右键菜单之间的共享状态。
//
// 为什么要走 Win32 窗口子类而不是 XAML 事件：cppwinrt 3.0.260818.1 生成的
// Microsoft.UI.Xaml.Input.h 里，**所有** delegate 的模板构造函数都漏了 inline
// （PointerEventHandler / RightTappedEventHandler …），
// 只有 Windows.Foundation.h 那批（RoutedEventHandler / TypedEventHandler）是好的。
// 结论：Microsoft.UI.* 的事件一律接不上（直接接 → LNK2019；
// 绕开 make_delegate → base.h 的 C2027「使用了未定义类型」）。
// 窗口过程是静态函数、拿不到 this，靠 SetWindowSubclass 的 refData 传这里。
struct ServiceMenuHost {
    winrt::Microsoft::UI::Xaml::Controls::ListView list{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout menu{ nullptr };
    winrt::Microsoft::UI::Xaml::FrameworkElement root{ nullptr };
    int rowCount = 0;   // 每次重建列表后同步，供命中测试做边界判断
};

// 模板实参必须是 ServiceMainWindow 自己。
// 写成 MainWindow 的话（改名时漏改过），在**只 include 了本头**的编译单元里
// MainWindow 会解析到同命名空间下的**投影类型** winrt::LingwuToolbox::MainWindow
// —— 那是外壳窗口，跟这里没关系 —— 于是 C++/WinRT 生成的
// root_implements<投影类型, ServiceMainWindow, composing, IComponentConnector>
// 根本没法构造，报 C2665，而且报错位置在 Microsoft.UI.Xaml.Markup.h 里，
// 离真正的原因隔着三层。
struct ServiceMainWindow : ServiceMainWindowT<ServiceMainWindow> {
    ServiceMainWindow();

private:
    // ---- 启动装配 ----
    void ApplyBackdrop();
    void WireEvents();

    // ---- 心跳 ----
    void OnTimerTick();
    void DrainController();

    // ---- 列表 ----
    void RebuildRows();
    void UpdateRow(winrt::LingwuToolbox::ServiceRow const& row,
                   lingwu::core::service::ServiceInfo const& info);
    void UpdateStatusBar();
    void UpdateSortIndicators();
    // 用户主动改变排序/筛选后调用：清掉重排节流，让下一帧立刻应用新顺序。
    void ForceReorderNext();

    // ---- 交互 ----
    void OnSortHeaderClick(size_t column);
    // 第二个形参（TextChangedEventArgs）刻意不收：它的 Reason() 原本用来
    // 挡程序性赋值，但本工程从不回写搜索框，那个判断是死的，反而让 UIA
    // 完全驱动不了这个控件。详见 .cpp 里 OnSearchChanged 的注释。
    void OnSearchChanged(
        winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender);
    // SelectionChanged 是 RoutedEventHandler，sender 是 IInspectable 而不是 ListView，
    // 所以这里直接用成员 ServiceList() 取列表。
    void OnListSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
    void OnRefreshClicked();
    void OnNewServiceClicked();
    void OnStartClicked();
    void OnStopClicked();
    void OnRestartClicked();
    void OnStartTypeApplyClicked();
    void OnDeleteClicked();
    void OnDeleteConfirmed();

    // ---- 结果呈现 ----
    // 把一个操作结果变成提示框。
    //
    // 【offerElevate 已废弃】
    // 工具箱默认以管理员身份运行，不再有「以管理员身份重新启动」这个入口。
    // 参数保留是为了不改动所有调用点，但它已经不再影响任何行为。
    void ShowOutcome(winrt::hstring const& title, winrt::hstring const& message,
                     bool offerElevate);

    // 删除确认。第三方服务要求逐字输入服务名，系统服务只要普通确认。
    void ShowDeleteConfirm(std::wstring const& name, bool thirdParty, bool running);

    // 当前选中服务的名字；没有选中返回空串。
    std::wstring SelectedServiceName() const;

    // 记录窗口位置与筛选/排序偏好，供下次启动恢复。
    void SaveWindowBounds();

    // 表头列序
    static constexpr size_t kColName = 0;
    static constexpr size_t kColState = 1;
    static constexpr size_t kColStartType = 2;

    lingwu::core::service::Controller controller_{};
    lingwu::core::Settings settings_{};

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

    // 两个对话框都**常驻复用**，不随调用创建。
    //
    // 原因有二：
    //   · ContentDialog 必须由持有者保活到关闭为止，用局部变量会在
    //     ShowAsync() 返回后立刻析构；
    //   · 同一个 XamlRoot 上不能同时挂两个 ContentDialog，复用天然规避。
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog confirm_{ nullptr };   // 删除确认
    winrt::Microsoft::UI::Xaml::Controls::ContentDialog notice_{ nullptr };    // 结果 / 提权提示

    // 删除确认框的面板。**构造函数里只建一次**，之后永不重建。
    //
    // 为什么不能每次弹出现建一个 StackPanel 再 Append：
    // 这里的 TextBox 是长期存活的成员，而 XAML 元素在同一时刻只能有一个父节点。
    // 第二次弹出时再把它 Append 进一个新面板，这一步会抛异常；
    // 异常逃出 AppWindow.Closing 处理器会让上面那句 a.Cancel(true) 作废 ——
    // 窗口照常关闭、进程直接退出，症状是「第一次点 X 正常，第二次程序就没了」。
    winrt::Microsoft::UI::Xaml::Controls::StackPanel deletePromptPanel_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::TextBlock deletePromptText_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::TextBox deletePromptInput_{ nullptr };

    // 同一个 XamlRoot 上当前是否已有对话框打开。
    // 开之前置 true、Closed 时置 false，缺一处就会永久锁死。
    std::shared_ptr<std::atomic<bool>> dialogOpen_ =
        std::make_shared<std::atomic<bool>>(false);

    // 删除流程的暂存状态（确认框与删除按钮之间的一次性交接）。
    std::wstring pendingDeleteName_{};
    bool pendingDeleteThirdParty_ = false;
    bool pendingDeleteStopFirst_ = false;

    // 新建服务窗口。**必须持有强引用**：XAML 框架不延长我们的生命周期，
    // 局部变量一旦析构窗口就跟着没了。常驻复用而不是每次新开。
    winrt::LingwuToolbox::CreateServiceWindow createWindow_{ nullptr };

    std::shared_ptr<ServiceMenuHost> menuHost_{};
    HWND hwnd_{ nullptr };

    // ---- 行池（列表容器只创建一次，靠这三者保持稳定）--------------------
    //
    // source_ 是**唯一**赋给 ListView.ItemsSource 的那个对象，全程不换。
    //
    // 【为什么绝不能换 ItemsSource —— lingwu-sysmon 花两轮实测换来的】
    //   ① 每次换一个新的 ItemsSource：ListView 销毁并重建全部已实现容器，
    //      私有内存 125.8 MB → 162.5 MB / 60 秒且不收敛。
    //   ② 不换、但「key 集合一变就换」—— 这一档更隐蔽。探针实测 60 秒内触发
    //      20 次整表重建，每次报废再重建 163 个容器。
    //      **差 1 行就重建 163 个容器，这个比例极端失衡。**
    //
    // 另外，ItemsSource 的元素类型必须是 IInspectable，这是硬约束：
    // 实测 single_threaded_vector<IInspectable> 正常，而 <hstring> 和
    // <ServiceRow> 一律抛 E_INVALIDARG(0x80070057)。
    //
    // 必须是 **IObservableVector**，不能是普通 IVector：
    // single_threaded_vector() 返回 IVector，SetAt 不发 VectorChanged，
    // 表现是「排序在数据上生效了、UI 一动不动」，极易误判成「排序没实现」。
    //
    // 重排用的是 **SetAt（replace）**，不是 RemoveAt + InsertAt：
    // 后者是结构性变更，会让 ItemsRepeater 彻底停止实现容器
    // （症状：数据 300 行、屏幕只剩 1 行）。
    winrt::Windows::Foundation::Collections::IObservableVector<
        winrt::Windows::Foundation::IInspectable> source_{ nullptr };
    std::unordered_map<std::wstring, winrt::LingwuToolbox::ServiceRow> rowByKey_{};
    std::vector<std::wstring> currentOrder_{};
    bool listBound_ = false;

    // ---- 重排节流 --------------------------------------------------------
    // 数值刷新（row.Update → Notify → 绑定引擎只改 TextBlock.Text，不碰容器）
    // 与顺序应用（SetAt → VectorChanged → ItemsRepeater 作废并重建槽位容器）
    // 是两条独立路径，开销差一个数量级。所以顺序应用降到每 N 毫秒一次。
    //
    // 【关键不变量】currentOrder_ 记录的是**已经显示在屏幕上**的顺序。
    // 节流跳过的帧绝不能更新 currentOrder_，否则下一帧比对时会把一个
    // 从未应用过的顺序当成已应用，真正该做的替换全部被跳过 ——
    // 症状是列表顺序再也不更新。
    static constexpr ULONGLONG kReorderIntervalMs = 1500;
    ULONGLONG lastOrderApplyTick_ = 0;

    std::wstring filter_{};
    lingwu::core::service::FilterMode filterMode_ = lingwu::core::service::FilterMode::All;
    lingwu::core::service::SortMode sortMode_ = lingwu::core::service::SortMode::NameAsc;
    size_t visibleCount_ = 0;
    size_t totalCount_ = 0;
    ULONGLONG lastRefreshTick_ = 0;

    // 最近一帧的原始快照，供 RebuildRows 过滤排序用。
    std::vector<lingwu::core::service::ServiceInfo> snapshot_{};

    // 两个 ComboBox 的「上次已知选中项」。本版 cppwinrt 的
    // SelectionChangedEventHandler 接不上（见 ServiceMenuHost 上方的说明），
    // 所以改用心跳里比对 SelectedIndex。
    int lastFilterIndex_ = -1;
    int lastStartTypeIndex_ = -1;

    // 选中行的详情是否已经填进面板。
    bool detailShown_ = false;

    // 后台线程活着吗。回调里绝不碰 this：窗口可能已经销毁。
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    std::wstring selectedName_{};
};

}  // namespace winrt::LingwuToolbox::implementation

namespace winrt::LingwuToolbox::factory_implementation {
struct ServiceMainWindow : ServiceMainWindowT<ServiceMainWindow, implementation::ServiceMainWindow> {};
}  // namespace winrt::LingwuToolbox::factory_implementation
