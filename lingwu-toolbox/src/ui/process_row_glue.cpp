// process_row_glue.cpp —— ProcessRow 的链接接线
//
// cppwinrt 为每个 runtimeclass 生成一个 <Type>.g.cpp，里面定义
//     void* winrt_make_LingwuToolbox_ProcessRow()
// 以及投影类 winrt::LingwuToolbox::ProcessRow 的默认构造函数。
// 而 module.g.cpp 只是**声明**前者再调用它 —— 所以必须有一个编译单元把
// <Type>.g.cpp 原样引入，否则链接期报
//     module.g.obj : error LNK2019: 无法解析的外部符号 winrt_make_LingwuToolbox_ProcessRow
//
// 为什么必须是**独立**编译单元，不能塞进 ProcessRow.h：
// ProcessRow.h 会被多个 TU 间接包含（实测 App.xaml.obj / MainWindow.xaml.obj /
// xaml_typeinfo.obj 三份都会看到它），而 <Type>.g.cpp 没有 include guard，
// 放在头里会直接 LNK2005「已经在此 .obj 中定义」。
//
// 顺序有讲究：先 ProcessRow.h（拿到 implementation / factory_implementation 两个
// 完整类型），再 ProcessRow.g.cpp（它要 make<factory_implementation::ProcessRow>()）。
#include "pch.h"

#include "ProcessRow.h"

#if __has_include("ProcessRow.g.cpp")
#include "ProcessRow.g.cpp"
#endif