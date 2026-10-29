#pragma once

#include <QtGlobal> // qreal

/// 聊天栏布局常量单一来源。
/// 此前同一数值以 static constexpr 散落三份（ChatSessionPage.cpp、NewChatPage.cpp、
/// ChatMsgEdit.cpp 的裸 800），任何一处调整都会破坏「输入框与消息流同栏」的构图对齐。
namespace LayoutConst {

/// 聊天栏宽度上限：NewChatPage 输入 dock、ChatSessionPage 消息列与底部输入组、
/// ChatMsgEdit 自身最大宽共用，保证三者同栏
inline constexpr int kColumnMaxWidth = 800;

/// 页面水平留白：resizeEvent 按 width() - 2*kSideMargin 计算可用栏宽，
/// 并与页面 setContentsMargins 的左右留白对齐
inline constexpr int kSideMargin = 35;

/// 用户气泡最大宽占可用栏宽系数：MessageBubbleWidget::refreshSize 对
/// availableContentWidth 的钳制乘数（用户气泡刻意窄于全栏，与助手气泡区分）
inline constexpr qreal kMsgColWidthFactor = 0.75;

/// 页面垂直留白（UI 优化 P2-D3）：三页此前各抄字面量——ChatSessionPage 与
/// SettingsPage 用 35、NewChatPage 英雄区用 24（更紧，让居中的英雄组视觉重心不偏移）。
/// 两个值都是有意不同，但「不同」必须被命名才看得出是决策而非疏漏；改值即改本头。
inline constexpr int kPageVMargin = 35;
inline constexpr int kHeroVMargin = 24;

/// 消息列内部间隙（P2-D3）：ChatSessionPage 的外层 spacing、空态/通知行的内边距与
/// ChatMsgEdit 工具集行同用 15，此前四处各抄一遍裸值。
inline constexpr int kColGap = 15;

/// 会话侧边栏固定宽度：SessionSidebar 自钳制 + ChatSessionPage 列宽计算扣除量
inline constexpr int kSidebarWidth = 280;

/// 消息列与侧栏之间的间隙：页面外层 QHBoxLayout 的 spacing，
/// 亦计入 ChatSessionPage::resizeEvent 的侧栏占用扣除（宽 + 隙为一个整块）
inline constexpr int kSidebarGap = 12;

/// 侧栏收起后浮动展开钮的边长（手动摆位的小按钮，ChatSessionPage 创建/定位）
inline constexpr int kSidebarRestoreBtnSize = 28;

/// 浮动展开钮距页面顶部的偏移（右上角定位，横向沿用 kSideMargin）
inline constexpr int kSidebarRestoreTop = 12;

/// 流式跟随的「近底」判定阈值（px）：用户滚动位置距底部小于此值视为已回到底部，
/// 恢复自动跟随并收起「回到底部」浮动钮（评审 B1：上翻不再被新内容粗暴拽回）
inline constexpr int kNearBottomPx = 80;

/// 「回到底部」浮动钮边长与距滚动区底部的内边距
inline constexpr int kJumpBottomBtnSize = 30;
inline constexpr int kJumpBottomBtnMargin = 12;

/// 导航列展开宽（UI 优化 P2-D1）：LiteHarness::initNavView 的 setViewWidth 与下方
/// kWindowMinWidth 的推导式同源。此前 256 只在 cpp 里裸写、注释里再手抄一遍，
/// 改宽不会报错但最小宽推导立刻失真（窄窗白屏假象的正是这条算式）
inline constexpr int kNavWidth = 256;

/// 主窗口最小尺寸（评审 A1）：导航 kNavWidth + 侧栏 kSidebarWidth + 隙 kSidebarGap
/// + 页面边距 2×kSideMargin 之外仍须留出可读的消息列，兜住「三列固定宽叠加把消息列
/// 钳到 0」的窄窗白屏假象。数值不变，仅把推导式里手抄的常数改为引用常量（P2-D1）
inline constexpr int kWindowMinWidth = 900;
inline constexpr int kWindowMinHeight = 600;

} // namespace LayoutConst
