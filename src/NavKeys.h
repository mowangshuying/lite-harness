#ifndef NAVKEYS_H
#define NAVKEYS_H

/*
    NavKeys.h —— 导航项 key 与堆叠页键的纯常量单源（只依赖 C++ 字面量）

    为什么从 NavItem.h 拆出来（P3-2）：这些键是「导航项 key 与 FluStackedLayout 页键
    必须成对一致」的契约，字面量曾散落在 LiteHarness.cpp 十余处，改一处漏一处就会让
    keyChanged 找不到页（表现为点了导航没反应）。收敛后仍有一个没人钉住的漂移面：
    SessionPrefix 与 SessionKeyFmt 必须同前缀，否则恢复会话与新建会话的键会撞车或错位
    （表现是「打开旧会话变成新会话」）。拆成纯头后 tst_navkeys 可以直接断言这条关系 ——
    原头依赖 FluVNavigationIconTextItem（Qt6::Gui），单测目标根本 include 不进来。
*/

namespace NavKey
{
inline constexpr char NewChatPage[] = "NewChatPage";   // 新建会话页（导航项 + 堆叠页键）
inline constexpr char SettingsPage[] = "SettingsPage"; // 设置页（导航项 + 堆叠页键）
inline constexpr char SessionsGroup[] = "SessionsGroup"; // Sessions 导航分组（子项挂载点）
inline constexpr char SessionPrefix[] = "Session_";    // 会话键前缀：恢复会话为 Session_+hex dataId
inline constexpr char SessionKeyFmt[] = "Session_%1";  // 会话键格式：新会话 Session_+自增号（arg 填充）
} // namespace NavKey

#endif // NAVKEYS_H