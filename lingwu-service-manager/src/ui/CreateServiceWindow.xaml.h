#pragma once

// 按官方 C++/WinRT 模板的写法，这里只引 cppwinrt 生成的 CreateServiceWindow.g.h。
// 那个头内部会用 __has_include 自动带上 CreateServiceWindow.xaml.g.h。
#include "CreateServiceWindow.g.h"

#include "pch.h"

#include "core/service_model.h"

namespace winrt::LingwuServiceManager::implementation {

// 新建服务窗口。独立 Window 而不是 ContentDialog：七个字段加就地校验提示
// 塞进对话框会被迫拉伸变形，而主窗口已经常驻复用了两个 ContentDialog。
struct CreateServiceWindow : CreateServiceWindowT<CreateServiceWindow> {
    CreateServiceWindow();

    // 回读用户填的字段。主窗口在 Closed 时调。
    //
    // 写成 7 个只读属性而不是一个带 7 个 [out] 形参的方法，是因为这套
    // MIDL 两种都写不出来（详见 MainWindow.idl 的长注释）：
    //   带 [out] -> MIDL5005 直接报错；不带 -> cppwinrt 生成 const&
    // 于是签名对不上，生成的头里报 C2664，看着像自己写错了。
    // 注意这 7 个都**不能**加 const：XAML 编译器为 x:Name 生成的访问器
    // （ServiceNameBox() 等）是非 const 成员，const 成员函数里调不动。
    winrt::hstring ServiceNameInput();
    winrt::hstring DisplayNameInput();
    winrt::hstring DescriptionInput();
    winrt::hstring BinaryPathInput();
    int32_t StartTypeChoice();
    winrt::hstring AccountInput();
    bool StartImmediatelyInput();

    // 窗口是被「创建」关的还是被「取消 / X」关的。主窗口在 Closed 回调里
    // 靠它决定要不要真的建服务 —— 填完表单点取消也建出来是不能接受的。
    bool Accepted();

    // 实时校验：返回空串表示通过，否则返回原因。
    // 「为什么禁用创建按钮」必须**就地**说明，不要点了才弹框。
    winrt::hstring ValidateName(winrt::hstring const& serviceName);
    winrt::hstring ValidateDisplayName(winrt::hstring const& displayName);
    winrt::hstring ValidateBinaryPath(winrt::hstring const& binaryPath);

    // 打开文件选择对话框。用户取消时返回空串。
    winrt::hstring PickBinaryFile();

private:
    // TextChanged 是 TextChangedEventHandler，属 Microsoft.UI.* → 接不上。
    // 所以改用心跳比对文本内容（与主窗口处理两个 ComboBox 同一套路）。
    void ValidateNow();

    std::wstring lastServiceName_{};
    std::wstring lastDisplayName_{};
    std::wstring lastBinaryPath_{};

    // 只有「创建」按钮会把它置 true。取消与 X 关闭都留 false。
    // 每 Activate 都要复位，否则上一次点过创建之后，随便开一次窗口
    // 再按 X 就会凭空建出一个服务。
    bool accepted_ = false;

    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

    // 校验文案常驻：TextBlock 每次重建的话，旧对象仍挂在视觉树上，
    // 换文案的瞬间会有一帧空白。
    winrt::Microsoft::UI::Xaml::Controls::TextBlock nameError_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::TextBlock displayError_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::TextBlock pathError_{ nullptr };
    winrt::Microsoft::UI::Xaml::Controls::TextBlock pathWarning_{ nullptr };

    // 三个内置账户的显示顺序与写入 CreateServiceRequest 的字面量。
    // 顺序即下拉索引，别在别处硬编码同一组常量。
    //
    // 【为什么是 inline constexpr 变量而不是 static constexpr 成员】
    // C++17 的 static constexpr 数据成员是隐式 inline 的，但那只覆盖
    // **字面量类型**；数组不是。写成 `static constexpr wchar_t const* kAccounts[3];`
    // 会报 C2789「必须初始化常量限定类型的对象」，而且就算补上定义，
    // 每个包含本头的 TU 还得各来一份。inline 变量两件事一起解决。
    inline static constexpr wchar_t const* kAccounts[3] = {
        L"LocalSystem",
        L"LocalService",
        L"NetworkService",
    };
};

}  // namespace winrt::LingwuServiceManager::implementation

// cppwinrt 的 module.g.cpp 会为每个 runtimeclass 生成工厂缓存条目，
// 所以 factory_implementation 必须存在，哪怕主窗口只用 make<>() 从不激活它。
// 少这一段就是 CreateServiceWindow.g.cpp 里三条错：成员不存在、
// 类型未声明、make<>() 找不到匹配的重载。
namespace winrt::LingwuServiceManager::factory_implementation {
struct CreateServiceWindow : CreateServiceWindowT<CreateServiceWindow, implementation::CreateServiceWindow> {};
}  // namespace winrt::LingwuServiceManager::factory_implementation
