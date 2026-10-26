#pragma once
//
// ProcessRow —— 主界面列表的行模型（WinRT runtimeclass 实现）
//
// 为什么不是普通结构体：x:Bind 要在**编译期**解析成员，而编译器只认元数据里
// 存在的类型。普通结构体不在任何 winmd 里，会直接报
//     WMC0909  Cannot resolve DataType local:ProcessRow
//     WMC1111  DataTemplates containing x:Bind need a DataType
// 两者都在本工程实测复现过。声明进 ProcessRow.idl 之后它才是一个真正的
// WinRT runtimeclass，经典 {Binding} 也才认得它。
//
// 为什么实现 INotifyPropertyChanged：行对象现在**跨帧存活**。MainWindow 每秒
// 只调用 Update() 改值并抛 PropertyChanged，列表容器不再被整体重建。
// 之前那种「每秒换一个新的 ItemsSource」实测会让私有内存从 125.8 MB 一路
// 涨到 162.5 MB 还不收敛（对照：关掉列表后 60 秒锁死在 91.4 MB）。
//
// 为什么还要实现 ICustomPropertyProvider：XAML 侧现在用**经典 {Binding}**
// 而不是 x:Bind。两者各缺一半，合起来才凑得齐——
//   · x:Bind：编译期能直读字段，渲染没问题；但它凭 winmd 元数据判断源是否
//     支持通知，而 INPC 是在 C++ 侧用模板参数实现的、没写进 IDL，
//     于是报 WMC1506 并把 OneWay 静默降级成一次性读取（数值 9 秒不动）。
//   · 经典 {Binding}：运行期订阅 INPC，纯 C++ 实现的 INPC 接得上；
//     但它不解析普通 WinRT 对象的属性 —— 行容器在、Items().Size() 正确、
//     状态栏也在刷新，列表就是一片空白。必须由对象自己提供
//     ICustomPropertyProvider（详见下面 GetCustomProperty 的注释）。
//
// ProcessRowT 的模板参数为什么要多带两个接口：
// cppwinrt 3.0.260818.1 生成的是
//     template <typename D, typename... I>
//     using ProcessRowT = ProcessRow_base<D, I...>;
// 而 ProcessRow_base<D, I...> : implements<D, LingwuToolbox::ProcessRow, I...>，
// 所以额外接口是通过**模板参数**追加的，不是再加一个基类。
//
// 头文件里就地实现，不单独开 .cpp：本类型只在 MainWindow.xaml.cpp 一处使用，
// 而 vtable 必须全程序唯一。

#include "pch.h"

// cppwinrt 生成的组件头。ProcessRow.g.h 里既有 ProcessRowT 模板，
// 也有 projection（winrt::LingwuToolbox::ProcessRow）。
#include "ProcessRow.g.h"

// ICustomProperty::Type() 要返回 Windows.UI.Xaml.Interop.TypeName，
// 那是 UWP 兼容层的一个 struct（hstring Name + TypeKind Kind），
// 属于 winrt/Windows.UI.Xaml.Interop.h，pch.h 的投影列表里没有。
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace winrt::LingwuToolbox::implementation {

// 被 XAML 绑定的文本属性。
enum class RowProp { Name, CpuText, MemText, CountText, IconText, PidText };

inline winrt::hstring RowPropName(RowProp p) noexcept {
    switch (p) {
        case RowProp::Name:      return L"Name";
        case RowProp::CpuText:   return L"CpuText";
        case RowProp::MemText:   return L"MemText";
        case RowProp::CountText: return L"CountText";
        case RowProp::IconText:  return L"IconText";
        case RowProp::PidText:   return L"PidText";
    }
    return L"";
}

// 只需要**函数**的前向声明，不再需要前置声明 RowCustomProperty 本身。
//
// 以前这里是个循环依赖：GetCustomProperty 的函数体要 make<RowCustomProperty>，
// 那时 RowCustomProperty 必须已完整；而 RowCustomProperty::GetValue 又要解引用
// ProcessRow 调 getter，那时 ProcessRow 必须已完整。两个都是成员函数体，
// 谁也没法在定义里看见对方，于是用这个自由函数当中间层。
//
// 现在 GetValue 只用引擎传进来的 target（QI 到投影类型取属性），
// **不再解引用 implementation::ProcessRow**，那条依赖消失了：
// RowCustomProperty 只依赖 RowProp，可以完整定义在 ProcessRow 之前。
// 但为了不打乱现有的文件结构（行模型在前、绑定描述对象在后），
// 这里仍然只给函数声明，定义留在文件末尾 —— 声明不需要参数类型完整。
//
// 注意 ProcessRow 的前向声明仍必须写：在这个
// namespace winrt::LingwuToolbox::implementation 里，光写 ProcessRow 会被
// 外层作用域的投影类型 winrt::LingwuToolbox::ProcessRow 抢先解析，
// 于是原型与实现对不上，报 C2664「无法将 implementation::ProcessRow*
// 转换为 ProcessRow*」。
struct ProcessRow;
winrt::Microsoft::UI::Xaml::Data::ICustomProperty MakeRowCustomProperty(RowProp prop);

struct ProcessRow
    : ProcessRowT<ProcessRow,
                  winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged,
                  winrt::Microsoft::UI::Xaml::Data::ICustomPropertyProvider> {
    using base_type = ProcessRowT<ProcessRow,
                                  winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged,
                                  winrt::Microsoft::UI::Xaml::Data::ICustomPropertyProvider>;

    ProcessRow() = default;

    ProcessRow(winrt::hstring name, winrt::hstring cpuText, winrt::hstring memText,
               winrt::hstring countText, winrt::hstring iconText, winrt::hstring pidText,
               double cpuNorm, uint64_t memBytes,
               uint32_t procs, uint32_t pidStart, uint32_t pidCount)
        : m_name(std::move(name)), m_cpuText(std::move(cpuText)),
          m_memText(std::move(memText)), m_countText(std::move(countText)),
          m_iconText(std::move(iconText)), m_pidText(std::move(pidText)),
          m_cpuNorm(cpuNorm), m_memBytes(memBytes), m_procs(procs),
          m_pidStart(pidStart), m_pidCount(pidCount) {}

    // ---- INotifyPropertyChanged ----------------------------------------
    // WinRT 事件用 add_/remove_ 对，不是 C# 那种访问器。
    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return m_changed.add(handler);
    }
    void PropertyChanged(winrt::event_token const& token) noexcept {
        m_changed.remove(token);
    }

    // ---- ICustomPropertyProvider ----------------------------------------
    //
    // 为什么非实现不可：微软文档《Binding 标记扩展》里写得很直白 ——
    //   「在 C++/WinRT 中使用 {Binding} 标记扩展，你需要实现
    //     ICustomPropertyProvider 和 ICustomProperty 接口。」
    // 纯 WinRT 对象不会像 C# 对象那样自带反射可绑属性；不实现这个接口，
    // 经典绑定**连第一次读取都拿不到值**，症状是列表区一片空白：
    // 行容器在、Items().Size() 正确、状态栏也在刷新，就是一个字都不显示。
    //
    // 这条路是被逼出来的：x:Bind 能解析（编译期直读字段），但判定不了
    // 通知支持 —— WMC1506「OneWay 绑定的某一步必须能抛通知」——
    // 因为 INotifyPropertyChanged 只在 C++ 侧用模板参数实现、没写进 IDL
    // （Windows App SDK 的 NuGet 包不提供 Microsoft.UI.Xaml.Data.idl 可 import）。
    // 于是 x:Bind 能渲染但数值不动，经典绑定能通知但解析不出属性。
    // 两个半截凑一起，本文件把缺的那一半补上。
    winrt::Microsoft::UI::Xaml::Data::ICustomProperty GetCustomProperty(
        winrt::param::hstring const& name) const {
        RowProp prop;
        if      (name == L"Name")      prop = RowProp::Name;
        else if (name == L"CpuText")   prop = RowProp::CpuText;
        else if (name == L"MemText")   prop = RowProp::MemText;
        else if (name == L"CountText") prop = RowProp::CountText;
        else if (name == L"IconText")  prop = RowProp::IconText;
        else if (name == L"PidText")   prop = RowProp::PidText;
        else return nullptr;   // 不认识的属性名必须返回 null，别编一个空属性出来
        // 只把「是哪个属性」交给描述对象。行对象由 GetValue 的 target 提供，
        // 所以这里不再需要 const_cast 拿自己的指针 —— 少一条悬空路径。
        return MakeRowCustomProperty(prop);
    }
    winrt::Microsoft::UI::Xaml::Data::ICustomProperty GetIndexedProperty(
        winrt::param::hstring const& /*name*/,
        winrt::Windows::UI::Xaml::Interop::TypeName const& /*type*/) const {
        return nullptr;   // 行不是索引器
    }
    winrt::hstring GetStringRepresentation() const { return m_name; }
    winrt::Windows::UI::Xaml::Interop::TypeName Type() const {
        // provider 的 Type 描述的是**行对象本身**，不是某个属性，
        // 所以必须是元数据里的运行时类名 + TypeKind::Metadata。
        //
        // 这里曾经错写成 Primitive/"String"（那是 ICustomProperty::Type 的
        // 写法），代价是启动约 5 秒后进程以
        //     Application Error / 模块 Microsoft.UI.Xaml.dll /
        //     异常代码 0xc000027b (STATUS_STOWED_EXCEPTION)
        // 崩溃 —— 窗口先正常显示、再毫无征兆地消失，XAML 把真实 HRESULT
        // 裹在 stowed exception 里，事件日志看不出成因。
        // 换成 Metadata + 真实类名后同样场景稳定运行，实测无异常。
        return { L"LingwuToolbox.ProcessRow",
                 winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata };
    }

    // ---- 只读属性 -------------------------------------------------------
    winrt::hstring Name()      const { return m_name; }
    winrt::hstring CpuText()   const { return m_cpuText; }
    winrt::hstring MemText()   const { return m_memText; }
    winrt::hstring CountText() const { return m_countText; }
    winrt::hstring IconText()  const { return m_iconText; }
    winrt::hstring PidText()   const { return m_pidText; }

    double  CpuNorm()  const { return m_cpuNorm; }
    uint64_t MemBytes() const { return m_memBytes; }
    uint32_t Procs()    const { return m_procs; }
    uint32_t PidStart() const { return m_pidStart; }
    uint32_t PidCount() const { return m_pidCount; }

    // ---- 原地更新 -------------------------------------------------------
    // 只有真正变了的字段才抛 PropertyChanged。
    //
    // 为什么必须逐字段比对：160 行 × 4 个单元格 = 每秒 640 次通知，
    // 而其中绝大多数帧只有 CPU 和内存两个数字在动，名字和进程数不变。
    // 无脑全量通知会让 x:Bind 每次都去写一遍 TextBlock.Text，
    // 实测那正是内存爬升的主要来源之一。
    void Update(winrt::hstring name, winrt::hstring cpuText, winrt::hstring memText,
                winrt::hstring countText, winrt::hstring iconText, winrt::hstring pidText,
                double cpuNorm, uint64_t memBytes,
                uint32_t procs, uint32_t pidStart, uint32_t pidCount) {
        if (m_name      != name)      { m_name      = std::move(name);      Notify(L"Name"); }
        if (m_cpuText   != cpuText)   { m_cpuText   = std::move(cpuText);   Notify(L"CpuText"); }
        if (m_memText   != memText)   { m_memText   = std::move(memText);   Notify(L"MemText"); }
        if (m_countText != countText) { m_countText = std::move(countText); Notify(L"CountText"); }
        if (m_iconText  != iconText)  { m_iconText  = std::move(iconText);  Notify(L"IconText"); }
        if (m_pidText   != pidText)   { m_pidText   = std::move(pidText);   Notify(L"PidText"); }
        // 下面五个数值字段**不发通知**。XAML 里只绑了上面四个文本属性
        // （GetCustomProperty 也只认这四个名字），排序在 core 层做、右键菜单
        // 只读选中项，所以这五条通知发出去没有任何订阅者会匹配。
        //
        // 曾经照发，代价不只是浪费：160 行 × 5 个字段 = 每秒约 1100 次
        // 跨 ABI 事件派发，每次都让绑定引擎白跑一遍「查名字 → 查不到 → 放弃」。
        // 实测 CPU↓ 排序下私有内存 164.4 MB、超 ~130 MB 红线，其中一份就是
        // 这份无效 churn。真要给它们加绑定，记得同时在 GetCustomProperty 里
        // 放行对应的属性名，否则照样一条都刷不动。
        m_cpuNorm   = cpuNorm;
        m_memBytes  = memBytes;
        m_procs     = procs;
        m_pidStart  = pidStart;
        m_pidCount  = pidCount;
    }

private:
    // 通知必须带**真实的属性名**。
    //
    // 之前这里传的是空字符串，注释写着「x:Bind 只关心这一行脏了」——
    // 那条规则在改成经典 {Binding} 之后完全不成立：绑定引擎拿属性名去
    // GetCustomProperty 里查对应项，传空名就是查不到，一条绑定都不会刷新。
    // 注意必须是非 const：winrt::event 的 operator() 不是 const 成员。
    void Notify(wchar_t const* propertyName) {
        if (m_changed) {
            m_changed(nullptr, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(
                                   propertyName));
        }
    }

    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_changed;

    winrt::hstring m_name;
    winrt::hstring m_cpuText;
    winrt::hstring m_memText;
    winrt::hstring m_countText;
    winrt::hstring m_iconText;
    winrt::hstring m_pidText;
    double   m_cpuNorm  = 0.0;
    uint64_t m_memBytes = 0;
    uint32_t m_procs    = 0;
    uint32_t m_pidStart = 0;
    uint32_t m_pidCount = 0;
};

// 单个自定义属性的描述对象。绑定引擎每条绑定各持有一个。
//
// 【为什么这里一个行指针都不存】曾经存的是裸 ProcessRow*，注释还写着
// 「行对象比绑定活得久」。三例 0xc0000005 证明这个假设是错的：
//   · RowCustomProperty::GetValue +0x10C   (t≈50s)
//   · RowCustomProperty::GetValue +0xC9    (t≈65s)
//   · memcpy +0xC6                          (t≈99s，读垃圾 HSTRING 后复制时踩空)
// 第三次崩在 memcpy 正是第二次那类故障的下游：从已释放的 ProcessRow 上读出
// 一个垃圾 HSTRING 句柄，box_value 复制它时才踩空。
//
// 根因：帧 N 里 row.Update() 抛 PropertyChanged，绑定引擎**只打脏标记**，
// 真正的 GetValue 推迟到之后的 layout pass；而同一帧的 SetAt / rowByKey_
// 重建会把已退出进程的行引用计数清零、当场释放。等引擎来读时对象已经没了。
// 进程启停越频繁，这个窗口被踩中的概率越高 —— 三次时点飘忽（50/65/99 秒）
// 而稳态能跑满 180 秒，正是这个特征。
//
// 【试过并否决的两条路】
// ① 让本对象持强引用（get_strong()）。会成环：行 → m_changed(event) →
//    delegate_array → PropertyChangedEventHandler 是强引用链
//    （base.h:5923 event::add → make_agile_delegate），引擎又持着我们，
//    于是 引擎 → RowCustomProperty → 行 → event → delegate → 引擎。
//    COM 没有循环回收，环不断就是永久泄漏，还会把已退出的行钉死在内存里。
// ② 退休缓冲区：把被挤出列表的行多留两帧。只是把窗口从「同帧」推到「两帧」，
//    节流后 SetAt 变成每 3 秒一次，绑定可能跨多个应用周期存活，两帧不够 ——
//    实测第 4 例崩溃（GetValue +0x184，t≈111s）依然复现。已删除。
//
// 【正解】GetValue 的 target 参数**就是引擎传进来的行对象**，而引擎调用时
// 必然持有它的强引用（否则它没法把指针交出来）。所以直接用 target 取当时的值，
// 一个指针都不用存：对象活着才可能被传进来，传进来就一定活着。
// target 为空（理论上不该发生）时返回 null，绑定读到空值，下一帧自愈，
// 不会崩。
struct RowCustomProperty
    : winrt::implements<RowCustomProperty,
          winrt::Microsoft::UI::Xaml::Data::ICustomProperty> {
    explicit RowCustomProperty(RowProp prop) noexcept : m_prop(prop) {}

    winrt::hstring Name() const { return RowPropName(m_prop); }

    // 四列都是字符串，一律报 Primitive + "String"（微软示例的写法）。
    winrt::Windows::UI::Xaml::Interop::TypeName Type() const {
        return { L"String", winrt::Windows::UI::Xaml::Interop::TypeKind::Primitive };
    }

    winrt::Windows::Foundation::IInspectable GetValue(
        winrt::Windows::Foundation::IInspectable const& target) const {
        // 只认行对象本身。QI 到投影类型而不是实现类：投影类型的 Name /
        // CpuText / MemText / CountText 都写在 ProcessRow.idl 里，
        // 而实现类的 getter 只有本工程内部能看到。
        const auto row = target.try_as<winrt::LingwuToolbox::ProcessRow>();
        if (!row) return nullptr;
        switch (m_prop) {
            case RowProp::Name:      return winrt::box_value(row.Name());
            case RowProp::CpuText:   return winrt::box_value(row.CpuText());
            case RowProp::MemText:   return winrt::box_value(row.MemText());
            case RowProp::CountText: return winrt::box_value(row.CountText());
            case RowProp::IconText:  return winrt::box_value(row.IconText());
            case RowProp::PidText:   return winrt::box_value(row.PidText());
        }
        return nullptr;
    }

    // 行是只读的：SetValue 不会被调用，实现为空即可，CanWrite() 也要报 false，
    // 否则引擎可能尝试 TwoWay 回写。
    void SetValue(winrt::Windows::Foundation::IInspectable const& /*target*/,
                  winrt::Windows::Foundation::IInspectable const& /*value*/) const {}

    winrt::Windows::Foundation::IInspectable GetIndexedValue(
        winrt::Windows::Foundation::IInspectable const& /*target*/,
        winrt::Windows::Foundation::IInspectable const& /*index*/) const {
        return nullptr;
    }
    void SetIndexedValue(winrt::Windows::Foundation::IInspectable const& /*target*/,
                         winrt::Windows::Foundation::IInspectable const& /*value*/,
                         winrt::Windows::Foundation::IInspectable const& /*index*/) const {}

    bool CanWrite() const { return false; }
    bool CanRead()  const { return true; }

private:
    RowProp m_prop;
};

// 中间层实现：只有到这里，两个类型才都已经完整，winrt::make 才构造得出来。
// 参数只要 RowProp：行对象由 GetValue 的 target 提供，本对象不持有它。
inline winrt::Microsoft::UI::Xaml::Data::ICustomProperty MakeRowCustomProperty(
    RowProp prop) {
    return winrt::make<RowCustomProperty>(prop);
}

}  // namespace winrt::LingwuToolbox::implementation

// cppwinrt 生成的 module.g.cpp 会为每个 runtimeclass 生成工厂缓存条目，
// 所以 factory_implementation 也必须存在，哪怕我们只用 make<>() 从不激活它。
// 少这一段就是链接期报一堆 unresolved external symbol。
namespace winrt::LingwuToolbox::factory_implementation {
struct ProcessRow : ProcessRowT<ProcessRow, implementation::ProcessRow> {};
}  // namespace winrt::LingwuToolbox::factory_implementation

// 注意：winrt_make_LingwuToolbox_ProcessRow 与投影类默认构造函数定义在
// cppwinrt 生成的 ProcessRow.g.cpp 里，**不要**在这里 include 它。
// 本头会被多个 TU 包含（经 pch.h → MainWindow.g.h → … 的链条），
// 放这里会得到 LNK2005「已经在此 .obj 中定义」。接线放在 process_row_glue.cpp。
