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

/// 会话侧边栏固定宽度：SessionSidebar 自钳制 + ChatSessionPage 列宽计算扣除量。
/// 与 kNavWidth 同宽（两栏统一为 225）——此前 280/256 两个值并存，左右两栏不等宽
inline constexpr int kSidebarWidth = 225;

/// 消息列与侧栏之间的间隙：页面外层 QHBoxLayout 的 spacing，
/// 亦计入 ChatSessionPage::resizeEvent 的侧栏占用扣除（宽 + 隙为一个整块）
inline constexpr int kSidebarGap = 12;

/// 侧栏收起后浮动展开钮的边长（手动摆位的小按钮，ChatSessionPage 创建/定位）
/// 侧栏收起后浮动展开钮的边长（P1-B 命中区下限 32px：28px 在触控板/高 DPI 下容易点偏）。
/// 与 #sidebarRestoreBtn 的 QSS 圆角（4px，非正圆）无联动，改值无需同步样式。
inline constexpr int kSidebarRestoreBtnSize = 32;

/// 浮动展开钮距页面顶部的偏移（右上角定位，横向沿用 kSideMargin）
inline constexpr int kSidebarRestoreTop = 12;

/// 流式跟随的「近底」判定阈值（px）：用户滚动位置距底部小于此值视为已回到底部，
/// 恢复自动跟随并收起「回到底部」浮动钮（评审 B1：上翻不再被新内容粗暴拽回）
inline constexpr int kNearBottomPx = 80;

/// 「回到底部」浮动钮边长与距滚动区底部的内边距
inline constexpr int kJumpBottomBtnSize = 32;
inline constexpr int kJumpBottomBtnMargin = 12;

/// 导航列展开宽（UI 优化 P2-D1）：LiteHarness::initNavView 的 setViewWidth 与下方
/// kWindowMinWidth 的推导式同源。此前 256 只在 cpp 里裸写、注释里再手抄一遍，
/// 改宽不会报错但最小宽推导立刻失真（窄窗白屏假象的正是这条算式）。
/// 与 kSidebarWidth 同宽（两栏统一为 225）
inline constexpr int kNavWidth = 225;

/// 主窗口最小尺寸（评审 A1）：导航 kNavWidth + 侧栏 kSidebarWidth + 隙 kSidebarGap
/// + 页面边距 2×kSideMargin 之外仍须留出可读的消息列，兜住「三列固定宽叠加把消息列
/// 钳到 0」的窄窗白屏假象。数值不变，仅把推导式里手抄的常数改为引用常量（P2-D1）
inline constexpr int kWindowMinWidth = 900;
inline constexpr int kWindowMinHeight = 600;

/// 设置卡右侧「修改/清除」钮的基值（P0 字号缩放）：实际尺寸经 UiScale::scaled 放大。
/// 此前 10 处各自写死 setFixedSize(64, 30)，英文文案（Change/Clear）+ 大字号下按钮
/// 文字被截且无省略号；单源后改一处即改全部，且与字号倍率同步。
inline constexpr int kCardActionWidth = 64;
inline constexpr int kCardActionHeight = 30;

/// 版本更新卡右侧「检查更新 / 下载并更新」两枚钮的宽基值（实际值经 UiScale::scaled 放大）。
/// **不复用 kCardActionWidth=64**：那档是为「修改/清除」两字文案定的，本卡文案是 4-5 字。
/// Qt 实测 naturalW（`QPushButton::sizeHint().width()`，挂真实 vendor FluPushButton.qss，
/// 选择器改写为 QPushButton 使其确实命中——用裸 QPushButton 挂原选择器时规则不命中，
/// 量出来的 naturalW 全是 75/117 一类的假数）：
///   修改 32 / 清除 32 / 检查更新 62 / 下载并更新 77 / Change 57 / Clear 39
///   Check for Updates 136 / Download && Update 151
/// **英文档必须按修正后的运行时串 `Download && Update` 量**。`.ts` 原写 `Download &amp; Update`，
/// XML 解码即单 `&`，QPushButton 把单 `&` 当助记符吃掉（实测 `shortcut()` 返回 `Alt+Space`），
/// 界面上显示成 "Download  Update"——于是 naturalW 只有 **138**。138 是**缺陷态**的宽度而非文案
/// 宽度，拿它当基线会系统性低估 13px（本头初稿即取 126/150 两档，都被这个假数带偏，已作废）。
/// 本批同批把 `.ts` 改成 `&amp;&amp;`，此后 151 才是真基线。
/// 字号由 vendor qss 钉死 15px：那份 qss 由控件自加载、不经本仓 UiScale::scaleQssFontSizes，
/// 故字形不随 uiFontScale 变，只有这里的框随倍率走——于是**倍率越低越紧**，80% 才是最坏档。
/// 160 在 100% 档覆盖修正后英文两档（151 之外余 9px）与中文两档，符合「装下英文即装下中文」。
/// **已知边界，登记而非假装解决**：80% 档框缩到 `scaledSize(160,80)=128px`，仍小于英文所需的
/// 151，裁 23px；三档全不裁需 **≥189**（`scaledSize(189,80)=151` 恰好达标，188 只给 150）。
/// 阈值一律按 `scaledSize` 的真实算式 `(base*percent+50)/100` 整数除法算，勿用纯 `ceil`；
/// 早先注释里的「≥172」是按缺陷态 138 推的，与修正后文案无关，同批作废。
/// 本批按用户裁决取 160，即「100% 与 200% 档英文不裁、80% 档英文仍裁」。
inline constexpr int kCardUpdateActionWidth = 160;

/// 版本更新卡右侧进度环的边长（s17.13）：实际值经 UiScale::scaled 放大，与同排按钮同步。
/// **不复用 FluProgressRing 的默认 60×60**——那是独立控件页的尺寸，塞进设置卡操作行会把
/// 卡片行高从 30 撑到 60，与同页其余六卡视觉不齐（正是本仓反复登记过的那类缺陷）。
/// 取 30 = kCardActionHeight：环与同排按钮等高、在 30 高的行里填满，视觉对齐。
/// **上界即行高 30**——任何 >30 的值都会把行高撑起来，重演上面那类缺陷；20 那档是空心的小环
/// 在行里不抢视觉，但用户反馈太小，改到与按钮等高这一档。
/// 环内不写字（百分比唯一出口仍是说明位文案），所以 30 这档不需要为文字留空间。
inline constexpr int kCardProgressRingSize = 30;

/// 输入框右侧发送/停止钮的边长（P3-3）：从 SendMsgButton.cpp 的 setFixedSize(30, 30)
/// 收敛而来。QSS 圆角取一半（15px）成正圆，改这个值必须同步 SendMsgButton.qss 的圆角。
/// 与 kCardActionHeight 同值但语义不同（那是设置卡动作钮的高），不合并。
inline constexpr int kSendBtnSize = 30;

/// 新建会话页中心图标的边长（P3-4）：从 NewChatPage.cpp 的 setFixedSize(45, 45) 收敛。
inline constexpr int kNewChatIconSize = 45;

// 同义常量防漂移（P3-6）：s17.3 把导航列与侧栏统一收窄到 225px，两个常量同值同义
// （都表示左侧栏宽），下次若只改一处会出现「导航列比侧栏宽/窄」的错位。编译期钉住。
static_assert(kSidebarWidth == kNavWidth,
              "sidebar width and nav width must stay equal (both = left column width)");

} // namespace LayoutConst
