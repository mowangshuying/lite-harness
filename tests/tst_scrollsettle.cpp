// 贴底算术与跟随态判据守卫（s18.5 实测坑登记）。
//
// 守什么：ScrollSettle.h 里的 range 算术、新件对布局 min 的贡献、两阶段落点、缺口、
// 跟随态判据。ChatSessionPage::evaluateFollowState 直调 isScrolledAway，故跟随态判据
// 与真代码绑定。
//
// 不守什么（诚实边界）：Qt 的滚动条 range **时序**——range 由
// QScrollAreaPrivate::updateScrollBars()（qscrollarea.cpp:145-194）写，其触发源是
// setWidget / LayoutRequest 与 StyleChange / 滚动件 QEvent::Resize / scrollarea
// resizeEvent / setWidgetResizable 五条，QLayout::activate() 的同步路径一条都不触发。
// 这条时序是 Qt Widgets 行为，测试目标只链 Qt6::Core（CMakeLists.txt:177），进不了
// ctest；守它的是会话 .temp 下的独立 Qt Widgets 探针（未入库、不进 ctest）。
// 本套件把「同栈落点必是旧 range」作为**模型断言**，不是对 Qt 的实测。

#include "TestHarness.h"
#include "ScrollSettle.h"
#include "LayoutConstants.h"

int tst_scrollsettle()
{
    using TestHarness::check;
    using ScrollSettle::rangeFor;
    using ScrollSettle::itemMinContribution;
    using ScrollSettle::sameStackLanding;
    using ScrollSettle::deferredLanding;
    using ScrollSettle::gap;
    using ScrollSettle::isScrolledAway;

    // ① range 算术：widgetResizable(true) 下滚动件被 resize 到 max(视口高, 布局 min)，
    //    故 range = max(0, 布局min − 视口高)
    check(rangeFor(200, 200) == 0, "rangeFor: 内容恰填满视口 -> 0");
    check(rangeFor(200, 199) == 0, "rangeFor: 内容不足视口 -> 0（不产生负 range）");
    check(rangeFor(200, 201) == 1, "rangeFor: 溢出 1px -> 1");
    check(rangeFor(200, 300) == 100, "rangeFor: 溢出 100px -> 100");
    check(rangeFor(0, 0) == 0, "rangeFor: 零视口零内容 -> 0");
    check(rangeFor(198, 315) == 117, "rangeFor: 探针 v8 实测形态（viewport=198 w=315 max=117）");

    // ② 新件对布局 min 的贡献：addWidget 那一刻新件是 hidden，QWidgetItem::minimumSize()
    //    首行 isEmpty() 短路返回 (0,0)（qlayoutitem.cpp:621-626 + 696）
    check(itemMinContribution(true, 32) == 0, "hidden 折叠卡对布局 min 贡献 0（isEmpty 短路）");
    check(itemMinContribution(false, 32) == 32, "show 后贡献全高 32");
    check(itemMinContribution(true, 200) == 0, "hidden 长卡贡献 0（与卡高无关）");
    check(itemMinContribution(false, 200) == 200, "show 后贡献全高 200");

    // ③ 真 app 口径：视口 198、内容已填满、插一张折叠卡（头部 32）+ 列 spacing
    const int viewport = 198;
    const int contentBefore = 198;
    const int cardHeader = 32;
    const int colGap = LayoutConst::kColGap;
    check(colGap == 15, "kColGap 真值 15（消息列 layout spacing）");
    check(cardHeader == 32, "折叠卡头部 32（CollapsibleBlock::kHeaderHeight，100% 字号档）");
    check(cardHeader + colGap == 47, "折叠卡缺口算式 32 + 15 = 47（docs 第三部分口径）");

    const int contentAfterStale = contentBefore + itemMinContribution(true, cardHeader) + colGap;
    const int contentAfterFresh = contentBefore + itemMinContribution(false, cardHeader) + colGap;
    check(contentAfterStale == 213, "同栈时布局 min 只涨 spacing（新件 hidden 贡献 0）");
    check(contentAfterFresh == 245, "下一轮布局 min 含新卡全高");

    check(sameStackLanding(viewport, contentBefore) == 0,
          "同栈贴底落点 = 插卡前的底部（range 未含新卡）");
    check(deferredLanding(viewport, contentAfterFresh) == 47,
          "补一拍落点 = 真底（range 已含新卡）");
    check(gap(viewport, contentBefore, contentAfterFresh) == 47,
          "长会话下缺口 = 新内容高度 + 列 spacing = 47");

    // ④ 探针口径（合成 200px 卡、spacing 0、视口 198、内容 100）——与真 app 口径分开，
    //    不混用：探针实测 gap=102 = 200 − 视口内 98px 空余，不是「卡高」
    const int pViewport = 198;
    const int pBefore = 100;
    const int pAfter = 100 + 200 + 0;
    check(sameStackLanding(pViewport, pBefore) == 0, "探针口径同栈落点 0（内容未填满视口）");
    check(deferredLanding(pViewport, pAfter) == 102, "探针口径补一拍落点 102");
    check(gap(pViewport, pBefore, pAfter) == 102, "探针实测 gap=102（200 − 视口内 98 空余）");
    check(gap(pViewport, pBefore, pAfter) != 200, "gap 不等于卡高——口径混用即此条会红");

    // ⑤ 跟随态判据（evaluateFollowState 直调本函数）：近底（距 maximum ≤ 阈值）=跟随
    const int near = LayoutConst::kNearBottomPx;
    check(near == 80, "kNearBottomPx 真值 80");
    check(isScrolledAway(0, 47, near) == false,
          "折叠卡缺口 47 < 80 -> 不被判离开（47px 插卡不会触发跟随抑制）");
    check(isScrolledAway(0, 102, near) == true,
          "长内容缺口 102 > 80 -> 被判离开（此后插卡不拽回、只保持回底钮）");
    check(isScrolledAway(102, 102, near) == false, "落在真底 -> 跟随");
    check(isScrolledAway(22, 102, near) == false, "边界：value == maximum − 80 -> 近底，不离开");
    check(isScrolledAway(21, 102, near) == true, "边界：value == maximum − 81 -> 离开");
    check(isScrolledAway(0, 0, near) == false, "range 为 0（内容不足视口）-> 永不判离开");

    // ⑥ 补一拍落点必等于 range（value == maximum 是「贴到真底」的定义）
    check(deferredLanding(viewport, contentAfterFresh)
              == rangeFor(viewport, contentAfterFresh),
          "补一拍落点 == range（value==maximum 即真底）");
    check(sameStackLanding(viewport, contentBefore)
              == rangeFor(viewport, contentBefore),
          "同栈落点 == 旧 range");

    // ⑦ 两阶段落点差 = 新件全高 + spacing（长会话填满视口时）
    check(deferredLanding(viewport, contentAfterFresh)
              - sameStackLanding(viewport, contentBefore) == cardHeader + colGap,
          "两阶段落点差 = 卡高 + spacing");

    return 0;
}
