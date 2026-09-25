#pragma once

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

} // namespace LayoutConst
