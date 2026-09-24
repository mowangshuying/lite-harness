#pragma once

// 导航/堆叠页键常量（单源化）：导航项 key 与 FluStackedLayout 页键必须成对一致，
// 字面量散落在 LiteHarness.cpp 十余处易漂移（改一处漏一处 → keyChanged 找不到页），统一收敛于此。
namespace NavKey
{
inline constexpr char NewChatPage[] = "NewChatPage";   // 新建会话页（导航项 + 堆叠页键）
inline constexpr char SettingsPage[] = "SettingsPage"; // 设置页（导航项 + 堆叠页键）
inline constexpr char SessionsGroup[] = "SessionsGroup"; // Sessions 导航分组（子项挂载点）
inline constexpr char SessionPrefix[] = "Session_";    // 会话键前缀：恢复会话为 Session_+hex dataId
inline constexpr char SessionKeyFmt[] = "Session_%1";  // 会话键格式：新会话 Session_+自增号（arg 填充）
}

// 导航分组项（Sessions 等）的最小派生：在 FluentUI 的 FluVNavigationIconTextItem 基础上，
// 补一个「移除子项」的能力，用于会话删除时把对应导航子项从分组中摘除。
// 基类 FluVNavigationIconTextItem 构造已把 (awesomeType,text,key) 委托到把 m_itemType 置为
// IconText 的构造（见 FluVNavigationIconTextItem.cpp），故本派生类无需重写任何类型/信号槽，
// getItemByKey 与 getAllItems 递归发现均照常工作；不新增 Q_OBJECT（无新信号槽，避免多余 moc）。
#include <FluVNavigationIconTextItem.h>

class NavItem : public FluVNavigationIconTextItem
{
public:
    NavItem(FluAwesomeType awesomeType, const QString &text, const QString &key, QWidget *parent = nullptr)
        : FluVNavigationIconTextItem(awesomeType, text, key, parent)
    {
    }

    // 从本分组移除 key 对应的子项：镜像 addItem 的逆操作——
    // addItem 做「item->m_parentItem=this; m_items.push_back; m_verticalLayout1->addWidget; isLong 时 m_arrow->show」。
    // 这里按 key 命中后：从 m_verticalLayout1 摘出、从 m_items 抹除、deleteLater。
    // 关键：子项构造时其 QWidget parent 即本分组（见 FluVNavigationView::insertIconTextItem 4 参版
    // createIconTextItem(...,parentItem)），故不可 setParent(nullptr)，否则会在 deleteLater 前
    // 一瞬被提升为顶层窗口而闪现在屏幕；仅从布局/集合摘除，实体交给 deleteLater（随父析构亦安全）。
    // 移除末项后隐藏展开箭头并按长导航重算分组高度，保持与 addItem 后的视觉对称。
    void removeChildItem(const QString &childKey)
    {
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
        {
            if ((*it)->getKey() != childKey)
                continue;
            FluVNavigationIconTextItem *child = *it;
            m_verticalLayout1->removeWidget(child);
            m_items.erase(it);
            child->deleteLater();
            if (m_items.empty() && m_arrow)
                m_arrow->hide();
            if (isLong())
                adjustItemHeight(this);
            return;
        }
    }
};
