#pragma once

// 折线 / 进度条的绘制助手。状态页与托盘面板共用同一份。
//
// 【为什么要抽出来，而不是各写一份】
// 状态页的 bento 磁贴与托盘面板的磁贴画的是同一套东西（mini 折线 + 进度条）。
// 两处各写一份，迟早会漂移 —— 状态页把线高改成 26 而面板还是 24，
// 用户一眼就能看出两个地方的 CPU 曲线「不是同一台机器」。
//
// 职责划分：core 层只负责算点（core/monitor/sparkline.h），
// 这里只负责把点贴到 Path / Border 上。两边都不碰对方的知识。

#include "core/monitor/history.h"
#include "core/monitor/sparkline.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.h>

namespace lingwu {
namespace ui {

// 折线在 Path 元素里的高度（DIP）。
//
// 【为什么不是控件高度】
// 折线画在 Path 元素里，Path 的 Stretch="None" —— 几何按原始尺寸画，
// 不被元素拉伸。所以必须让「几何的纵坐标范围」与元素高度对得上，
// 否则线会画到元素外面或者只占一半。
// 留 2 DIP 余量是为了线宽（1.5~1.6 DIP）不被裁掉。
//
// 这是状态页磁贴用的高度（宿主容器也是 24 高）。
// 托盘面板的磁贴更小，宿主容器只有 14 高 —— 那时由调用方把高度传进来，
// 否则线会**画到容器外面**（Path 不裁剪）。
inline constexpr double kSparkHeight = 24.0;

// 折线 → Geometry。
//
// 【为什么不能直接用路径字符串】
// Path.Data 的类型是 Geometry，而 Geometry 在运行期**没有**「从字符串构造」的
// API ——「M x,y L x,y」这种写法只有 XAML 解析期才认（写进 .xaml 里可以，
// 在 C++ 里赋值给 Data 就不行）。所以这里手工拼 PathFigure + LineSegment。
inline winrt::Microsoft::UI::Xaml::Media::Geometry BuildLineGeometry(
        const std::vector<std::pair<double, double>>& pts) {
    namespace Media = winrt::Microsoft::UI::Xaml::Media;
    if (pts.size() < 2) return Media::Geometry(nullptr);
    auto geo = Media::PathGeometry();
    auto fig = Media::PathFigure();
    fig.StartPoint(winrt::Windows::Foundation::Point{
        static_cast<float>(pts[0].first), static_cast<float>(pts[0].second) });
    auto segs = fig.Segments();
    for (std::size_t i = 1; i < pts.size(); ++i) {
        auto seg = Media::LineSegment();
        seg.Point(winrt::Windows::Foundation::Point{
            static_cast<float>(pts[i].first), static_cast<float>(pts[i].second) });
        segs.Append(seg);
    }
    geo.Figures().Append(fig);
    return geo;
}

// 把一条序列画到 Path 上。
//
// 宽度取**宿主 Grid**实际的布局宽度（不是 Path 自己的）：
// Path 的 Stretch="None"，它按几何的原始尺寸绘制，于是 Path 的 DesiredSize
// 等于几何尺寸 —— 拿它去量会变成「用输出宽度去算输入宽度」的循环。
// 宿主 Grid 才是磁贴里那个固定高度、宽度随窗口变化的容器。
//
// 布局还没完成时宿主宽度是 0，此时什么都不画 —— 下一秒的刷新会补上。
//
// height 是几何的纵坐标范围（**逻辑像素**），必须与宿主容器的高度一致。
// 默认取 kSparkHeight（状态页磁贴）；托盘面板的磁贴更小，由调用方传入。
inline void SetSpark(winrt::Microsoft::UI::Xaml::FrameworkElement const& host,
                     winrt::Microsoft::UI::Xaml::Shapes::Path const& p,
                     const lingwu::core::monitor::Series& series,
                     double minSpan,
                     double height = kSparkHeight) {
    namespace Core = lingwu::core::monitor;
    const double w = host.ActualWidth();
    if (!(w > 0.0)) return;
    if (!(height > 0.0)) return;

    std::vector<double> all(Core::kSeriesCapacity);
    const std::size_t n = series.CopyOldestFirst(all.data(), all.size());
    all.resize(n);
    // 只取最近的一段（与 core 的自动纵轴版本同口径）
    std::size_t begin = 0;
    if (all.size() > Core::kSparkMaxPoints) begin = all.size() - Core::kSparkMaxPoints;
    const std::vector<double> window(all.begin() + begin, all.end());
    // 点数不足 2 个**不拦** —— core 会画一条基线（plan.md §3.7）。
    // 拦掉的话，磁贴里那块固定高度的折线区会是一片空白，
    // 看起来像「功能坏了」，而不是「数据还没攒够」。
    double lo = 0.0, hi = 0.0;
    Core::ComputeSparkScale(window, minSpan, &lo, &hi);
    const auto coords = Core::BuildSparklinePoints(window, w, height, lo, hi);
    p.Data(BuildLineGeometry(coords));
}

// 进度条：按比例设置填充块的宽度。
//
// 【为什么要在 C++ 里量宽度】
// 填充块是磁贴里的一个 Border，它的宽度必须等于「轨道宽度 × 比例」。
// 轨道宽度只有布局完成后才知道（ActualWidth），
// 而 XAML 里没有「按父级宽度百分比设置子元素宽度」的机制
// ——Stretch 会把子元素拉满，缩放变换又会把圆角一起压扁。
// 所以这里直接量 ActualWidth 再写 Width。
inline void SetBar(winrt::Microsoft::UI::Xaml::FrameworkElement const& host,
                   winrt::Microsoft::UI::Xaml::Controls::Border const& fill,
                   double ratio) {
    const double w = host.ActualWidth();
    if (!(w > 0.0)) return;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    fill.Width(w * ratio);
}

}  // namespace ui
}  // namespace lingwu
