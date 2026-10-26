// sparkline.h —— 磁贴 mini 折线的几何计算（header-only，纯算术）
//
// 【为什么放在 core】
// 这里只做「一串采样值 → 一条 SVG 风格的路径数据」，不碰任何 XAML 类型。
// 于是它可以被 --selftest 直接覆盖 —— 而画折线最容易出的错
// （顺序反了、丢的是最新而不是最旧、纵轴把平坦的噪声放大成锯齿）
// 恰恰都是「界面上看着有点怪、但不会报任何错」的那种，
// 只能靠单元断言钉住。
//
// 【坐标约定】
// 输出的是 SVG path data 语法（"M x,y L x,y …"），
// Microsoft.UI.Xaml.Shapes.Path 的 Data 吃的就是同一套语法。
// 坐标系与 XAML 一致：左上为原点，**y 向下**。
// 所以「数值越大越靠上」这件事由本模块在换算时完成（y = h - …），
// 调用方不需要再翻转。
//
// 【为什么宽度高度由调用方传进来，而不是这里写死】
// 仓库铁律：布局交给 XAML 与系统 DPI，不写裸像素常量。
// 控件的实际尺寸（DIP）由布局算出，这里只负责把数据映射进去，
// 于是同一个模块在任何缩放比例、任何磁贴尺寸下都成立。
#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace lingwu {
namespace core {
namespace monitor {

// 折线的两种形态。
enum class SparklineKind {
    Line,   // 纯折线
    Area,   // 面积：折线 + 闭合到基线
};

// 一条折线最多画多少个点。
//
// 为什么要有上限：磁贴宽度有限（DIP），画 200 个点意味着每点不到 0.5 像素，
// 既看不出细节又白费 CPU。取最近的 40 个点足够看出趋势，
// 而 64 点的环形缓冲正好能提供这个窗口。
inline constexpr std::size_t kSparkMaxPoints = 40;

// 纵轴自适应范围。
//
// 返回 range = max - min，并把实际使用的上下界写进 outMin / outMax
// （传 nullptr 就不写）。
//
// 【为什么不能简单地按 min/max 缩放】
// 采样值带量化噪声（CPU 的排序键量化到 0.01%，显示精度也是 0.01%）。
// 一条恒定的 11% 内存占用，按 min/max 缩放后会被放大成满屏锯齿 ——
// 看着很热闹，实际什么信息都没有，还会让用户以为内存在剧烈波动。
// 所以极差小于 minSpan 时，给一个**以当前值为中心**的窗口，
// 让线画成一条平线。这是刻意的：平线才是真相。
//
// minSpan 的量纲由调用方按指标给（百分制指标给 10，速率指标给 0）。
double ComputeSparkScale(const std::vector<double>& points, double minSpan,
                          double* outMin, double* outMax);

// 生成折线上的点（坐标已换算成控件坐标系，y 向下）。
//
// 【为什么单独给一个「点」的版本】
// XAML 侧画折线要的是 Geometry（Path.Data），而 Geometry 在 C++ 里
// **不能**用「M x,y L x,y」这种字符串构造 —— 字符串形式只有 XAML 解析期才认，
// 运行期没有 API 能把路径字符串变成 Geometry。
// 所以这里额外给一份点坐标，UI 侧拿它拼 PathFigure + LineSegment。
//
// 而下面的字符串版本保留着：它是给 selftest 钉住「坐标换算对不对」用的
// （顺序反了、丢的是最新而不是最旧、纵轴把平坦噪声放大成锯齿，
//  这三种错都是「看着有点怪但不会报任何错」的，只能靠单元断言钉住）。
//
// 两份输出共用同一段换算代码，不会算出两条不一样的线。
//
// 点数不足 2 个时画一条**基线**（规格见 plan.md §3.7「失败与恢复语义」：
// 「sparkline 点数不足 → 只画已有的点；点数 < 2 时画一条基线」）。
//
// 【为什么不是「干脆不画」】
// 折线区是磁贴里一块**固定高度**的容器。什么都不画的话那块地方是一片空白，
// 看起来像「功能坏了」，而不是「数据还没攒够」。采样间隔是设置窗口里的
// 合法档位（可以选 5 秒），攒够 2 个点要 10 秒 —— 这段时间用户看到的就是空白。
std::vector<std::pair<double, double>> BuildSparklinePoints(
    const std::vector<double>& points,
    double width, double height,
    double minV, double maxV);

// 生成路径数据（显式纵轴）。
//
// points  按时间顺序（最旧 → 最新），最多取末尾 kSparkMaxPoints 个点。
// width / height 是控件实际尺寸（DIP）。
// minV / maxV 是纵轴上下界。退化区间（hi <= lo）会把线放在垂直中间，
// 而不是画到最底下 —— 最底下是「值为 0」的样子，不是「没有变化」的样子。
//
// 点数不足 2 个时画一条基线（与 BuildSparklinePoints 同一套规则）：
// 一个点 → 落在那个值的位置上；一个点都没有 → 落在垂直中间。
std::wstring BuildSparklineData(const std::vector<double>& points,
                                double width, double height,
                                SparklineKind kind,
                                double minV, double maxV);

// 生成路径数据（自动纵轴）。
//
// minSpan 的含义见 ComputeSparkScale：极差太小时给一个居中窗口，
// 把平坦的噪声压成一条平线而不是放大成锯齿。
inline std::wstring BuildSparklineDataAuto(const std::vector<double>& points,
                                            double width, double height,
                                            SparklineKind kind,
                                            double minSpan) {
    // 只取最近的一段再算范围 —— 纵轴要跟画出来的那一段对齐，
    // 否则窗口里最老的一个点可能落在画布之外。
    std::size_t begin = 0;
    if (points.size() > kSparkMaxPoints) begin = points.size() - kSparkMaxPoints;
    const std::vector<double> window(points.begin() + begin, points.end());

    double lo = 0.0, hi = 0.0;
    ComputeSparkScale(window, minSpan, &lo, &hi);
    return BuildSparklineData(window, width, height, kind, lo, hi);
}

// ---------------------------------------------------------------- 实现

inline double ComputeSparkScale(const std::vector<double>& points, double minSpan,
                                double* outMin, double* outMax) {
    if (points.empty()) {
        if (outMin) *outMin = 0.0;
        if (outMax) *outMax = 1.0;
        return 1.0;
    }

    double lo = 0.0, hi = 0.0;
    bool first = true;
    for (double v : points) {
        if (std::isnan(v) || std::isinf(v)) continue;   // 坏点直接跳过
        if (first) { lo = hi = v; first = false; }
        else {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (first) {   // 全是坏点
        if (outMin) *outMin = 0.0;
        if (outMax) *outMax = 1.0;
        return 1.0;
    }

    double span = hi - lo;
    if (span < minSpan) {
        const double center = (lo + hi) * 0.5;
        lo = center - minSpan * 0.5;
        hi = center + minSpan * 0.5;
        span = minSpan;
    }
    if (outMin) *outMin = lo;
    if (outMax) *outMax = hi;
    return span;
}

inline std::wstring BuildSparklineData(const std::vector<double>& points,
                                       double width, double height,
                                       SparklineKind kind,
                                       double minV, double maxV) {
    if (!(width > 0.0) || !(height > 0.0)) return L"";

    const auto pts = BuildSparklinePoints(points, width, height, minV, maxV);
    if (pts.empty()) return L"";

    std::wstring out;
    out.reserve(pts.size() * 24 + 64);
    wchar_t buf[96];
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i == 0) {
            swprintf_s(buf, L"M %.1f,%.1f", pts[i].first, pts[i].second);
        } else {
            swprintf_s(buf, L" L %.1f,%.1f", pts[i].first, pts[i].second);
        }
        out += buf;
    }

    // 面积形态闭合到基线。
    //
    // 【为什么点数不足时闭合要跳过】
    // 基线是一条水平线，闭合到基线会围出一个**整块的矩形** ——
    // 一块填满的色块比空白的折线区更像「画错了」。
    // 数据不够的时候只画线，不画面积。
    if (kind == SparklineKind::Area && points.size() >= 2) {
        // 闭合到基线：最后一列 → 右下 → 左下 → 回到起点
        const double firstX = pts.front().first;
        const double firstY = pts.front().second;
        const double lastX  = pts.back().first;
        swprintf_s(buf, L" L %.1f,%.1f L %.1f,%.1f L %.1f,%.1f Z",
                   lastX, height, firstX, height, firstX, firstY);
        out += buf;
    }

    return out;
}

// 折线上的点。坐标换算的**唯一**实现处 ——
// 字符串版本与点版本都从这里取，两条线永远不会算出不一样的结果。
inline std::vector<std::pair<double, double>> BuildSparklinePoints(
    const std::vector<double>& points,
    double width, double height,
    double minV, double maxV) {
    std::vector<std::pair<double, double>> out;
    if (!(width > 0.0) || !(height > 0.0)) return out;

    // ---- 点数不足 2 个：画一条基线（plan.md §3.7）----
    //
    // 【两种情况的落点不一样】
    //   · 有 1 个点：画在**这个值**的位置上。值是知道的，只是还没有趋势。
    //   · 一个点都没有：画在**垂直中间**。垂直最底下是「值为 0」的样子，
    //     而「还没有数据」不等于「值是 0」——
    //     这个区分沿用本文件里退化区间的既有约定（见下方 hi <= lo 的处理）。
    if (points.size() < 2) {
        double y = height * 0.5;
        if (points.size() == 1) {
            double lo = minV;
            double hi = maxV;
            if (hi <= lo) {
                const double center = lo;
                lo = center - 0.5;
                hi = center + 0.5;
            }
            double v = points[0];
            if (std::isnan(v) || std::isinf(v)) v = lo;
            y = height - (v - lo) / (hi - lo) * height;
            if (y < 0.0) y = 0.0;
            if (y > height) y = height;
        }
        out.emplace_back(0.0, y);
        out.emplace_back(width, y);
        return out;
    }

    // 只取最近的一段
    std::size_t begin = 0;
    if (points.size() > kSparkMaxPoints) begin = points.size() - kSparkMaxPoints;
    const std::size_t n = points.size() - begin;

    double lo = minV;
    double hi = maxV;
    if (hi <= lo) {
        // 退化区间：把线放在垂直中间，而不是画到最底下（那是「值为 0」的样子）
        const double center = lo;
        lo = center - 0.5;
        hi = center + 0.5;
    }
    const double span = hi - lo;

    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        double v = points[begin + i];
        if (std::isnan(v) || std::isinf(v)) v = lo;   // 坏点贴到下界，不画断线
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double x = t * width;
        double y = height - (v - lo) / span * height;
        if (y < 0.0) y = 0.0;
        if (y > height) y = height;
        out.emplace_back(x, y);
    }
    return out;
}

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
