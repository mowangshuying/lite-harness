// tst_layoutconst.cpp —— 布局常量自洽（src/LayoutConstants.h，纯 QtCore）
// 被测：全部尺寸为正、命中区下限、窗口最小宽与列宽关系、同义常量相等。
//
// 为什么必须测：布局常量改错没有编译错误，只在特定窗口尺寸下露馅（窄窗白屏、
// 三列叠加把消息列钳到 0、按钮被截字）。而这些常量分散在十余处被引用，改错一个
// 不会立刻崩，只会「看起来有点怪」—— 冒烟运行最容易漏掉的正是这类。
//
// 零夹具依赖，任何环境全量执行。

#include "TestHarness.h"

#include "LayoutConstants.h"

namespace {

using TestHarness::check;

} // namespace

int tst_layoutconst()
{
    const int before = TestHarness::failCount();

    // ---- ① 全部尺寸为正 ----
    check(LayoutConst::kColumnMaxWidth > 0, "layout: column max width positive");
    check(LayoutConst::kSideMargin > 0, "layout: side margin positive");
    check(LayoutConst::kPageVMargin > 0, "layout: page v margin positive");
    check(LayoutConst::kHeroVMargin > 0, "layout: hero v margin positive");
    check(LayoutConst::kColGap > 0, "layout: column gap positive");
    check(LayoutConst::kSidebarWidth > 0, "layout: sidebar width positive");
    check(LayoutConst::kSidebarGap > 0, "layout: sidebar gap positive");
    check(LayoutConst::kNavWidth > 0, "layout: nav width positive");
    check(LayoutConst::kWindowMinWidth > 0, "layout: window min width positive");
    check(LayoutConst::kWindowMinHeight > 0, "layout: window min height positive");
    check(LayoutConst::kCardActionWidth > 0, "layout: card action width positive");
    check(LayoutConst::kCardActionHeight > 0, "layout: card action height positive");
    check(LayoutConst::kSendBtnSize > 0, "layout: send button size positive");
    check(LayoutConst::kNewChatIconSize > 0, "layout: new chat icon size positive");

    // ---- ② 命中区下限（P1-B 的契约，与 tst_uiux 同源但此处覆盖全部可点钮）----
    check(LayoutConst::kSidebarRestoreBtnSize >= 32, "layout: restore btn hit target >= 32");
    check(LayoutConst::kJumpBottomBtnSize >= 32, "layout: jump bottom btn hit target >= 32");

    // ---- ③ 窗口最小宽必须容得下「导航 + 侧栏 + 隙 + 两倍边距」----
    // 否则三列固定宽叠加会把消息列钳到 0（窄窗白屏）
    check(LayoutConst::kWindowMinWidth > LayoutConst::kNavWidth + LayoutConst::kSidebarWidth,
          "layout: min window width > nav + sidebar");
    check(LayoutConst::kWindowMinWidth > 2 * LayoutConst::kSideMargin,
          "layout: min window width > 2 * side margin");

    // ---- ④ 消息列宽必须大于左右留白（否则列宽为负）----
    check(LayoutConst::kColumnMaxWidth > 2 * LayoutConst::kSideMargin,
          "layout: column max width > 2 * side margin");

    // ---- ⑤ 列宽比例在 (0, 1] 内（>1 会让消息列溢出容器）----
    check(LayoutConst::kMsgColWidthFactor > 0.0 && LayoutConst::kMsgColWidthFactor <= 1.0,
          "layout: msg column width factor in (0, 1]");

    // ---- ⑥ 同义常量相等（P3-6 的编译期断言在头里，这里做行为级复核）----
    check(LayoutConst::kSidebarWidth == LayoutConst::kNavWidth,
          "layout: sidebar width == nav width");

    // ---- ⑦ 正圆条件：圆形钮的尺寸必须是偶数（半径 = 尺寸/2 取整）----
    check(LayoutConst::kSendBtnSize % 2 == 0, "layout: send btn even size (perfect circle)");
    check(LayoutConst::kJumpBottomBtnSize % 2 == 0, "layout: jump bottom btn even size (perfect circle)");

    return TestHarness::failCount() - before;
}