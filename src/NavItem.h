#pragma once

// 导航/堆叠页键常量已抽到 NavKeys.h（纯 QtCore，P3-2）：本头只剩需要 Qt6::Gui 的派生类。
#include "NavKeys.h"

#include <FluIconUtils.h>
#include <FluThemeUtils.h>

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

    // 把分组置于「子项可见」态：显示子项容器、逻辑态与箭头字形一并对齐，再按可见子项重算本体高度。
    // 高度维护的不变量是「本体固定高度 = 图标行 + 当前**可见**的子项容器」，而 adjustItemHeight
    // 不看容器是否显示，onItemClicked 又是「翻转」语义（起点为「逻辑已展开但容器未显示」的内部态
    // ——点过零子项分组就会留下这个态——它会先走 Collapsing 把容器藏回去，随后子项的 onItemClicked
    // 仍把不可见行算回本体），两条路都堵不住，故这里直接落到唯一自洽的终态。
    void revealChildren()
    {
        if (m_items.empty())
            return;                     // 无子项可露：容器保持隐藏，本体仍是 30，自洽
        m_wrapWidget2->show();
        m_isDown = false;
        setState(FluVNavigationState::Expanded);
        if (m_arrow)
        {
            m_arrow->setIcon(FluIconUtils::getFluentIcon(FluAwesomeType::ChevronUp,
                                                       FluThemeUtils::getUtils()->getTheme()));
            m_arrow->show();
        }
        adjustItemHeight(this);
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
            // 判据与 createSession / renameSession 同源（s17.13.1）：= 「子项容器此刻是否真在显示」。
            // isLong() 只说明导航带处于宽态，本体此刻仍可能是折叠的（wrapWidget2 隐藏）；
            // 折叠态调 adjustItemHeight 会把不可见子项的总高算进本体固定高度——删一条会话反而把
            // 「会话」那一行撑高、下缘下移。容器没在显示时不动本体高度才是对的。
            // 本函数只在窗口 show 之后（用户右键删除）跑到，isVisible() 这里可靠。
            if (isLong() && getWrapWidget2()->isVisible())
                adjustItemHeight(this);
            return;
        }
    }
};
