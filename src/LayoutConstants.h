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

/// 会话侧边栏固定宽度：SessionSidebar 自钳制 + ChatSessionPage 列宽计算扣除量
inline constexpr int kSidebarWidth = 280;

/// 消息列与侧栏之间的间隙：页面外层 QHBoxLayout 的 spacing，
/// 亦计入 ChatSessionPage::resizeEvent 的侧栏占用扣除（宽 + 隙为一个整块）
inline constexpr int kSidebarGap = 12;

/// 侧栏收起后浮动展开钮的边长（手动摆位的小按钮，ChatSessionPage 创建/定位）
inline constexpr int kSidebarRestoreBtnSize = 28;

/// 浮动展开钮距页面顶部的偏移（右上角定位，横向沿用 kSideMargin）
inline constexpr int kSidebarRestoreTop = 12;

} // namespace LayoutConst
