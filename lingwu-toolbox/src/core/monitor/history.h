// history.h —— 磁贴趋势用的时间序列环形缓冲（header-only）
//
// 【为什么需要它】
// 状态页的 bento 磁贴要画 mini 折线（CPU / 内存 / 网速 / 磁盘活动 / 健康分）。
// 折线需要的是「最近 N 个采样点构成的序列」，而 Snapshot 里只有**当前这一帧**
// 的标量 —— 必须有人把历次采样攒起来，否则每次刷新画的都是一条水平直线。
//
// 【为什么放在 core 而不是 UI】
// 攒历史的时机是「采样线程每周期 push 一次」，与 UI 无关。
// 放 UI 侧就得让 UI 自己按定时器 push，等于把采样节奏复制一份到 UI ——
// UI 刷新比采样慢会丢点、比采样快会插重复点，两边迟早对不上。
//
// 【为什么是定长数组而不是 std::deque / std::vector】
// 窗口固定 64 个点（默认 1 秒采样 → 约 1 分钟）。定长数组让 Series 平凡可拷贝，
// 于是 History 能整个塞进 Snapshot 而**一次堆分配都不产生** ——
// Snapshot 每秒整体拷贝一次，不能带一堆堆分配进去。
#pragma once

#include <array>
#include <cstddef>

namespace lingwu {
namespace core {
namespace monitor {

// 单条序列的点数上限。默认采样间隔 1 秒 → 约 1 分钟的窗口。
//
// 为什么不做成可配置的：窗口长度变了，折线的「x 轴代表多长时间」也跟着变，
// 不同磁贴之间的横轴就不再可比（CPU 折线覆盖 1 分钟、内存覆盖 2 分钟，
// 并排放着看会误读）。固定窗口让所有磁贴的横轴含义一致。
inline constexpr std::size_t kSeriesCapacity = 64;

// 单条时间序列的环形缓冲。
class Series {
public:
    Series() = default;

    // 写入一个新点。满了以后覆盖最旧的。
    void Push(double value) {
        buf_[head_] = value;
        head_ = (head_ + 1) % kSeriesCapacity;
        if (count_ < kSeriesCapacity) ++count_;
    }

    void Clear() {
        head_  = 0;
        count_ = 0;
    }

    // 有效条数（还没把窗口填满时就是已写入的次数）。
    std::size_t Size() const { return count_; }

    static constexpr std::size_t Capacity() { return kSeriesCapacity; }

    // 最新值。空序列返回 0。
    double Last() const {
        if (count_ == 0) return 0.0;
        return buf_[IndexOf(count_ - 1)];
    }

    double Min() const {
        double v = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const double x = buf_[IndexOf(i)];
            if (i == 0 || x < v) v = x;
        }
        return v;
    }

    double Max() const {
        double v = 0.0;
        for (std::size_t i = 0; i < count_; ++i) {
            const double x = buf_[IndexOf(i)];
            if (i == 0 || x > v) v = x;
        }
        return v;
    }

    // 极差。折线按它做纵向缩放；恒为 0 说明这条线是平的，
    // 调用方据此决定是画一条中线还是留白（画一条上下抖动的噪声线没有信息量）。
    double Range() const { return Max() - Min(); }

    // 按时间顺序（最旧 → 最新）拷贝到 out，返回实际拷贝条数。
    //
    // out 容量不足时**只保留最近的 maxCount 条**，而不是丢掉尾部 ——
    // 折线要的是最新的那一段，丢掉尾部等于画出一段过期数据。
    std::size_t CopyOldestFirst(double* out, std::size_t maxCount) const {
        if (!out || maxCount == 0) return 0;
        const std::size_t n = (count_ < maxCount) ? count_ : maxCount;
        const std::size_t skip = count_ - n;      // 丢掉最旧的 skip 条
        for (std::size_t i = 0; i < n; ++i) out[i] = buf_[IndexOf(skip + i)];
        return n;
    }

private:
    // 第 i 个点（i 从最旧=0 起）在环形数组里的物理下标。
    std::size_t IndexOf(std::size_t i) const {
        const std::size_t oldest = (head_ + kSeriesCapacity - count_) % kSeriesCapacity;
        return (oldest + i) % kSeriesCapacity;
    }

    std::array<double, kSeriesCapacity> buf_{};
    std::size_t head_  = 0;   // 下一个写入位置
    std::size_t count_ = 0;   // 有效条数
};

// 状态页磁贴要画趋势的那几条序列。
//
// 为什么是「几个固定序列」而不是「键值对 map」：磁贴要画的图是**写死在页面里的**
// （CPU / 内存 / 网络 / 磁盘 / 健康分），每条序列的消费者都是确定的代码路径。
// 用 map 的话每次画图都要按字符串查一遍，再加一层类型擦除。
struct History {
    Series cpu;        // 整机 CPU 占用率（%）
    Series mem;        // 物理内存占用率（%）
    Series netDown;    // 下行（字节/秒）
    Series netUp;      // 上行（字节/秒）
    Series disk;       // 磁盘活动率（%）
    Series health;     // 健康分（0..100）
};

}  // namespace monitor
}  // namespace core
}  // namespace lingwu
