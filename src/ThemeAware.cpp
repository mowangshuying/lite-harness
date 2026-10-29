#include "ThemeAware.h"

#include "UiScale.h" // 只读启动快照，不现取 ini（生效时机与正文面同一条，见 UiScale.h）
#include <FluStyleSheetUtils.h>
#include <FluThemeUtils.h>

#include <QStyle>
#include <QWidget>

void ThemeAware::bind(const QString &qssFileName, QWidget *widget,
                      const std::function<void()> &extraRefresh)
{
    if (!widget)
        return;

    auto apply = [widget, qssFileName, extraRefresh](FluTheme theme) {
        // 字号缩放（P1-11）：本函数是**带字号 QSS** 的收口点（bind 13 处 + 三个折叠卡
        // 经 initTheme 汇入 = 16 处），故缩放只在这一处做——比在每处 bind 传参、或把
        // 87 条 font-size 抽成变量都更单源。
        // 另有三处刻意不走本收口点：BasePage.cpp:17（setQssByFileName）、
        // LiteHarness.cpp:591/598（NavAlign/ScrollBarAlign 追加块，须拼到 FluentUI 自身
        // 样式表尾部，语义与「整份替换」不同）。**这三份 QSS 实测零条字号声明**
        // （grep font-size|font: 均为 0），故绕开缩放不产生「部分文字不放大」的半生效，
        // 无需为此扩收口点；但若将来有人往这三个文件加字号，就必须同步接进来。
        // 读原文的路径拼法与 FluStyleSheetUtils 内部完全同源
        // （dir + 主题名 + 文件名），doForQrcQssText 也照抄其调用序列：本仓 QSS
        // 现无 ../res/ 引用（grep 核实），留着是为将来有人加图片引用时行为不变。
        //
        // 仍走 scheduleBatchUpdate 而非 widget->setStyleSheet：主题切换时
        // FluThemeUtils::setTheme 会开批处理并靠 applyBatchedUpdates 统一 flush，
        // 直接 setStyleSheet 会绕过批处理与 lastApplied 去重（切换期重复 polish）。
        // theme 形参在缩放路径里用不上（getThemeName 自取当前主题），保留给
        // setQssByFileName 的原有分支语义。
        Q_UNUSED(theme);
        const QString path = FluStyleSheetUtils::getUtils()->getStyleSheetDir()
                             + FluThemeUtils::getThemeName() + QLatin1Char('/') + qssFileName;
        QString qss = FluStyleSheetUtils::getQssByFileName(path);
        FluStyleSheetUtils::doForQrcQssText(qss);
        qss = UiScale::scaleQssFontSizes(qss, UiScale::runtimeFontScalePercent());
        FluStyleSheetUtils::scheduleBatchUpdate(widget, qss);
        // 组件特有行为放在 QSS 重载之后：依赖新样式级联的子级刷新
        // （如 ChatSessionPage 对气泡按 #msgBrowser[role] 重 polish）必须
        // 等页面级 QSS 就位后再做，否则读到的仍是旧主题级联。
        if (extraRefresh)
            extraRefresh();
        // 兜底重 polish 本体：extraRefresh 可能在 QSS 之后改动态属性
        // （如 CollapsibleBlock::refreshIcons 写 collapsed），而 Qt 不会因
        // setProperty 自动重解析属性选择器；且 setStyleSheet 收到与当前
        // 相同的样式串时会被 Qt 短路跳过重建。unpolish/polish 是属性选择器
        // 生效的确定路径，代价可忽略。
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    };

    // 首次应用与主题切换走同一条序列，保证「首载即与联动后状态一致」
    apply(FluThemeUtils::getUtils()->getTheme());

    QObject::connect(FluThemeUtils::getUtils(), &FluThemeUtils::themeChanged,
                     widget, [apply](FluTheme theme) { apply(theme); });
}
