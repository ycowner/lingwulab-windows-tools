#pragma once
//
// ServiceRow —— 主界面列表的行模型（WinRT runtimeclass 实现）
//
// 为什么不是普通结构体：x:Bind 要在**编译期**解析成员，而编译器只认元数据里
// 存在的类型。普通结构体不在任何 winmd 里，会直接报
//     WMC0909  Cannot resolve DataType local:ServiceRow
//     WMC1111  DataTemplates containing x:Bind need a DataType
// 声明进 ServiceRow.idl 之后它才是一个真正的 WinRT runtimeclass。
//
// 为什么实现 INotifyPropertyChanged：行对象**跨帧存活**。MainWindow 每两秒
// 只调用 Update() 改值并抛 PropertyChanged，列表容器不再被整体重建。
//
// 为什么还要实现 ICustomPropertyProvider：XAML 侧用的是**经典 {Binding}**
// 而不是 x:Bind。两者各缺一半，合起来才凑得齐——
//   · x:Bind：编译期能直读字段，渲染没问题；但它凭 winmd 元数据判断源是否
//     支持通知，而 INPC 是在 C++ 侧用模板参数实现的、没写进 IDL，
//     于是报 WMC1506 并把 OneWay **静默降级成一次性读取**
//     （症状是状态栏在变、列表里状态 9 秒纹丝不动）。
//   · 经典 {Binding}：运行期订阅 INPC，纯 C++ 实现的 INPC 接得上；
//     但它不解析普通 WinRT 对象的属性 —— 行容器在、Items().Size() 正确、
//     状态栏也在刷新，列表就是一片空白。必须由对象自己提供
//     ICustomPropertyProvider。
//
// ServiceRowT 的模板参数为什么要多带两个接口：
// cppwinrt 生成的是
//     template <typename D, typename... I>
//     using ServiceRowT = ServiceRow_base<D, I...>;
// 而 ServiceRow_base<D, I...> : implements<D, LingwuToolbox::ServiceRow, I...>，
// 所以额外接口是通过**模板参数**追加的，不是再加一个基类。
//
// 头文件里就地实现，不单独开 .cpp：本类型只在 MainWindow.xaml.cpp 一处使用，
// 而 vtable 必须全程序唯一。

#include "pch.h"

// cppwinrt 生成的组件头。ServiceRow.g.h 里既有 ServiceRowT 模板，
// 也有投影（winrt::LingwuToolbox::ServiceRow）。
#include "ServiceRow.g.h"

// ICustomProperty::Type() 要返回 Windows.UI.Xaml.Interop.TypeName，
// 那是 UWP 兼容层的一个 struct（hstring Name + TypeKind Kind），
// 属于 winrt/Windows.UI.Xaml.Interop.h，pch.h 的投影列表里没有。
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace winrt::LingwuToolbox::implementation {

// 五个被 XAML 绑定的文本属性。
enum class ServiceRowProp { Name, DisplayNameText, StateText, StartTypeText, PidText };

inline winrt::hstring ServiceRowPropName(ServiceRowProp p) noexcept {
    switch (p) {
        case ServiceRowProp::Name:            return L"Name";
        case ServiceRowProp::DisplayNameText: return L"DisplayNameText";
        case ServiceRowProp::StateText:       return L"StateText";
        case ServiceRowProp::StartTypeText:   return L"StartTypeText";
        case ServiceRowProp::PidText:         return L"PidText";
    }
    return L"";
}

// 只需要**函数**的前向声明，不再需要前置声明 ServiceRowCustomProperty 本身。
//
// 这里曾经是个循环依赖：GetCustomProperty 的函数体要 make<ServiceRowCustomProperty>，
// 那时 ServiceRowCustomProperty 必须已完整；而 ServiceRowCustomProperty::GetValue 又要
// 拿到行对象，那时行类型必须已完整。两个都是成员函数体，谁也没法在定义里
// 看见对方，于是用这个自由函数当中间层。
//
// 现在 GetValue 只用引擎传进来的 target（QI 到投影类型取属性），
// **不持有行对象**，那条依赖就没了。
//
// 注意 ServiceRow 的前向声明仍必须写：在这个
// namespace winrt::LingwuToolbox::implementation 里，光写 ServiceRow 会被
// 外层作用域的投影类型 winrt::LingwuToolbox::ServiceRow 抢先解析，
// 于是原型与实现对不上。
struct ServiceRow;
winrt::Microsoft::UI::Xaml::Data::ICustomProperty MakeServiceRowCustomProperty(ServiceRowProp prop);

struct ServiceRow
    : ServiceRowT<ServiceRow,
                  winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged,
                  winrt::Microsoft::UI::Xaml::Data::ICustomPropertyProvider> {
    using base_type = ServiceRowT<ServiceRow,
                                  winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged,
                                  winrt::Microsoft::UI::Xaml::Data::ICustomPropertyProvider>;

    ServiceRow() = default;

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
    winrt::Microsoft::UI::Xaml::Data::ICustomProperty GetCustomProperty(
        winrt::param::hstring const& name) const {
        ServiceRowProp prop;
        if      (name == L"Name")            prop = ServiceRowProp::Name;
        else if (name == L"DisplayNameText") prop = ServiceRowProp::DisplayNameText;
        else if (name == L"StateText")       prop = ServiceRowProp::StateText;
        else if (name == L"StartTypeText")   prop = ServiceRowProp::StartTypeText;
        else if (name == L"PidText")         prop = ServiceRowProp::PidText;
        else return nullptr;   // 不认识的属性名必须返回 null
        // 只把「是哪个属性」交给描述对象。行对象由 GetValue 的 target 提供，
        // 所以这里不需要 const_cast 拿自己的指针 —— 少一条悬空路径。
        return MakeServiceRowCustomProperty(prop);
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
        // 写成 Primitive/"String"（那是 ICustomProperty::Type 的写法）的代价是
        // 启动约 5 秒后进程以 0xc000027b (STATUS_STOWED_EXCEPTION) 崩在
        // Microsoft.UI.Xaml.dll —— 窗口先正常显示、再毫无征兆地消失。
        return { L"LingwuToolbox.ServiceRow",
                 winrt::Windows::UI::Xaml::Interop::TypeKind::Metadata };
    }

    // ---- 只读属性 -------------------------------------------------------
    winrt::hstring Name()            const { return m_name; }
    winrt::hstring DisplayNameText() const { return m_displayName; }
    winrt::hstring StateText()       const { return m_stateText; }
    winrt::hstring StartTypeText()   const { return m_startTypeText; }
    winrt::hstring PidText()         const { return m_pidText; }

    winrt::hstring Key()             const { return m_name; }
    uint32_t StateValue()      const { return m_stateValue; }
    uint32_t StartTypeValue()  const { return m_startTypeValue; }

    // ---- 原地更新 -------------------------------------------------------
    // 只有真正变了的字段才抛 PropertyChanged。
    //
    // 为什么必须逐字段比对：300 行 × 5 个单元格 = 每轮 1500 次通知，
    // 而绝大多数轮里只有状态与 PID 在动，名字和启动类型不变。
    // 无脑全量通知会让绑定引擎每次都去写一遍 TextBlock.Text，
    // 实测那是内存爬升的主要来源之一。
    void Update(winrt::hstring name, winrt::hstring displayName,
                winrt::hstring stateText, winrt::hstring startTypeText,
                winrt::hstring pidText, uint32_t stateValue, uint32_t startTypeValue) {
        if (m_name            != name)            { m_name            = std::move(name);            Notify(L"Name"); }
        if (m_displayName     != displayName)     { m_displayName     = std::move(displayName);     Notify(L"DisplayNameText"); }
        if (m_stateText       != stateText)       { m_stateText       = std::move(stateText);       Notify(L"StateText"); }
        if (m_startTypeText   != startTypeText)   { m_startTypeText   = std::move(startTypeText);   Notify(L"StartTypeText"); }
        if (m_pidText         != pidText)         { m_pidText         = std::move(pidText);         Notify(L"PidText"); }
        // 下面三个数值字段**不发通知**：XAML 里没绑它们
        // （GetCustomProperty 也只认上面五个名字），排序与筛选在 C++ 侧做。
        // 照发只是每次白白让绑定引擎跑一遍「查名字 → 查不到 → 放弃」。
        m_stateValue     = stateValue;
        m_startTypeValue = startTypeValue;
    }

private:
    // 通知必须带**真实的属性名**。传空字符串的话绑定引擎拿名字去
    // GetCustomProperty 里查不到，一条绑定都不会刷新。
    // 注意必须是非 const：winrt::event 的 operator() 不是 const 成员。
    void Notify(wchar_t const* propertyName) {
        if (m_changed) {
            m_changed(nullptr, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(
                                   propertyName));
        }
    }

    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_changed;

    winrt::hstring m_name;
    winrt::hstring m_displayName;
    winrt::hstring m_stateText;
    winrt::hstring m_startTypeText;
    winrt::hstring m_pidText;
    uint32_t m_stateValue     = 1;
    uint32_t m_startTypeValue = 3;
};

// 单个自定义属性的描述对象。绑定引擎每条绑定各持有一个。
//
// 【为什么这里一个行指针都不存】
// 第 N 帧 Update() 抛 PropertyChanged 时，绑定引擎**只打脏标记**，
// 真正的 GetValue 推迟到之后的 layout pass；而同一帧的 SetAt / rowByKey_
// 重建可能已把某些行释放。等引擎来读时指针已悬空 —— lingwu-sysmon 实测
// 180 秒崩 4 次 0xc0000005，且故障点全在这个 GetValue。
//
// 试过并否决的两条路：
// ① 持强引用（get_strong()）→ 成环。行 → event → delegate_array →
//    PropertyChangedEventHandler 是强引用链（base.h 的 event::add →
//    make_agile_delegate），引擎又持着我们，于是成环。COM 没有循环回收。
// ② 退休缓冲区（多留两帧）→ 只把窗口从「同帧」推到「两帧」。
//
// 【正解】GetValue 的 target 参数**就是引擎传进来的行对象**，而引擎调用时
// 必然持有它的强引用（否则它没法把指针交出来）。所以直接用 target 取当时的值，
// 一个指针都不用存：对象活着才可能被传进来，传进来就一定活着。
// target 为空时返回 null，下一帧自愈，不崩。
struct ServiceRowCustomProperty
    : winrt::implements<ServiceRowCustomProperty,
          winrt::Microsoft::UI::Xaml::Data::ICustomProperty> {
    explicit ServiceRowCustomProperty(ServiceRowProp prop) noexcept : m_prop(prop) {}

    winrt::hstring Name() const { return ServiceRowPropName(m_prop); }

    // 五列都是字符串，一律报 Primitive + "String"。
    winrt::Windows::UI::Xaml::Interop::TypeName Type() const {
        return { L"String", winrt::Windows::UI::Xaml::Interop::TypeKind::Primitive };
    }

    winrt::Windows::Foundation::IInspectable GetValue(
        winrt::Windows::Foundation::IInspectable const& target) const {
        // 只认行对象本身。QI 到投影类型而不是实现类：投影类型的五个文本属性
        // 都写在 ServiceRow.idl 里，而实现类的 getter 只有本工程内部能看到。
        const auto row = target.try_as<winrt::LingwuToolbox::ServiceRow>();
        if (!row) return nullptr;
        switch (m_prop) {
            case ServiceRowProp::Name:            return winrt::box_value(row.Name());
            case ServiceRowProp::DisplayNameText: return winrt::box_value(row.DisplayNameText());
            case ServiceRowProp::StateText:       return winrt::box_value(row.StateText());
            case ServiceRowProp::StartTypeText:   return winrt::box_value(row.StartTypeText());
            case ServiceRowProp::PidText:         return winrt::box_value(row.PidText());
        }
        return nullptr;
    }

    // 行是只读的：SetValue 不会被调用，CanWrite() 也要报 false，
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
    ServiceRowProp m_prop;
};

// 中间层实现：只有到这里，两个类型才都已经完整，winrt::make 才构造得出来。
// 参数只要 ServiceRowProp：行对象由 GetValue 的 target 提供，本对象不持有它。
inline winrt::Microsoft::UI::Xaml::Data::ICustomProperty MakeServiceRowCustomProperty(
    ServiceRowProp prop) {
    return winrt::make<ServiceRowCustomProperty>(prop);
}

}  // namespace winrt::LingwuToolbox::implementation

// cppwinrt 生成的 module.g.cpp 会为每个 runtimeclass 生成工厂缓存条目，
// 所以 factory_implementation 也必须存在，哪怕我们只用 make<>() 从不激活它。
// 少这一段就是链接期报一堆 unresolved external symbol。
namespace winrt::LingwuToolbox::factory_implementation {
struct ServiceRow : ServiceRowT<ServiceRow, implementation::ServiceRow> {};
}  // namespace winrt::LingwuToolbox::factory_implementation

// 注意：winrt_make_LingwuServiceManager_ServiceRow 与投影类默认构造函数定义在
// cppwinrt 生成的 ServiceRow.g.cpp 里，**不要**在这里 include 它。
// 本头会被多个 TU 包含（经 pch.h → MainWindow.g.h → … 的链条），
// 放这里会得到 LNK2005「已经在此 .obj 中定义」。接线放在 service_row_glue.cpp。
