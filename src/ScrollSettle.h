#pragma once

#include <QtGlobal>

// 会话消息流「贴底」的算术单源（s18.5 实测坑登记）。
//
// 本头**只**承载本页自己拥有的算术，不承载 Qt 内部时序——QScrollArea 的滚动条 range
// 由 QScrollAreaPrivate::updateScrollBars()（qscrollarea.cpp:145-194）写，其触发源是
// setWidget / LayoutRequest 与 StyleChange / 滚动件 QEvent::Resize / scrollarea 自身
// resizeEvent / setWidgetResizable 五条（249/277/305/316/357），`QLayout::activate()`
// 的同步路径一条都不触发（doResize() 即 q->setGeometry(contentsRect())，不改父件尺寸）。
// 这条时序是 Qt 行为，测试目标只链 Qt6::Core（CMakeLists.txt:177），进不了 ctest，
// 故本头不试图守它——守它的是会话 .temp 下的独立 Qt Widgets 探针（未入库）。
//
// 本头守的是：range 的算术、新件对布局 min 的贡献、两阶段落点、缺口、跟随态判据。
// ChatSessionPage::evaluateFollowState 直调 isScrolledAway，故跟随态判据与真代码绑定。

namespace ScrollSettle {

// QScrollArea widgetResizable(true) 下 range 的算术：
// 滚动件被 resize 到 max(视口高, 布局 min)，故 range = max(0, 布局min − 视口高)。
inline int rangeFor(int viewportHeight, int contentMinHeight)
{
    return contentMinHeight > viewportHeight ? contentMinHeight - viewportHeight : 0;
}

// 新件对布局 min 的贡献：新件在 addWidget 那一刻是 hidden（new 出来的件默认隐藏，
// QLayout::addChildWidget（qlayout.cpp:837）的 needShow 要等事件循环才 show），
// QWidgetItem::minimumSize()（qlayoutitem.cpp:621-626）首行 `if (isEmpty()) return
// QSize(0,0)`，isEmpty()（qlayoutitem.cpp:696）= isHidden && !retainSizeWhenHidden()
// → 贡献 0。注意 0 不是 qSmartMinSize 给的（它在 minSize.height()>0 时必返回该值，
// qlayoutengine.cpp:308-335）。
inline int itemMinContribution(bool hidden, int fixedHeight)
{
    return hidden ? 0 : fixedHeight;
}

// 同栈贴底落点：range 尚未含新卡，视野停在「插卡前的底部」。
inline int sameStackLanding(int viewportHeight, int contentMinBefore)
{
    return rangeFor(viewportHeight, contentMinBefore);
}

// 补一拍落点：下一轮事件循环里 range 已含新卡，视野落在真底。
inline int deferredLanding(int viewportHeight, int contentMinAfter)
{
    return rangeFor(viewportHeight, contentMinAfter);
}

// 缺口 = 真底 − 同栈落点。长会话（内容已填满视口）下缺口 = 新内容高度 + 列 spacing。
inline int gap(int viewportHeight, int contentMinBefore, int contentMinAfter)
{
    return deferredLanding(viewportHeight, contentMinAfter)
           - sameStackLanding(viewportHeight, contentMinBefore);
}

// 跟随态判据（ChatSessionPage::evaluateFollowState 的算术，真代码直调本函数）：
// 近底（距 maximum ≤ nearBottomPx）= 回到跟随；离底 = 抑制自动滚动、只保持回底钮。
inline bool isScrolledAway(int value, int maximum, int nearBottomPx)
{
    return value < maximum - nearBottomPx;
}

} // namespace ScrollSettle
