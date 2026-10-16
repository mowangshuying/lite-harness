#include "ChatStatusStrip.h"
#include "ThemeAware.h"
#include "UsageTokens.h"

#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QVector>

// ============================================================================
// 文件内辅助控件（无自定义信号槽，故不带 Q_OBJECT；所有 tr() 文案由
// ChatStatusStrip 组好后注入，保证翻译上下文统一挂在本类名下）
// ============================================================================

/// 计量文字行：**单行**——占用数字 + 消耗段 + 速度段。
/// 状态灯不在本行：三态灯整盏归会话侧栏「状态」节（见 SessionSidebar.h），本行只放
/// 「侧栏收起也必须常驻」的计量读数——两处同时亮灯会在同一时刻给出两个读数。
/// 占用**条**也不在本行（它归侧栏「上下文」节）；本行只承载文字，一行装不下时
/// 按各段自然宽比例压缩省略（tooltip 保留全文）。
/// 三段各自空账即整段隐藏（不显示 0 骗人）；三段全空时由属主 ChatStatusStrip 把
/// 本行连同分隔线一起隐藏，避免输入框下杵着一条没有内容的横线。
class StripRow : public QWidget {
public:
    explicit StripRow(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("stripRow"));
        setFixedHeight(26);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(kSpacing);
        // 占用数字段（原占用条下方的数字行，条归侧栏后数字随文字留在本行）
        m_numbers = new QLabel(this);
        m_numbers->setObjectName(QStringLiteral("ctxNumbers"));
        // 初值即隐藏：三段全空时 isEmpty() 才为真，属主据此把整条（含分隔线）收起；
        // 若这里留一个「可见但无文字」的标签，空状态条会算成「有内容」而常驻一条空线
        m_numbers->setVisible(false);
        // 消耗段：与占用数字**语义正交**（占用=窗口现在多大、会被后续回读覆盖；
        // 消耗=已经花掉多少、只增不减）。同色阶但更淡一档，避免与占用数字抢焦点。
        m_consumption = new QLabel(this);
        m_consumption->setObjectName(QStringLiteral("ctxConsumption"));
        m_consumption->setVisible(false);
        // 速度段：与消耗段同层级、同色阶，但语义正交——消耗回答「已经花掉多少」，
        // 速度回答「模型吐字有多快」。空账整段隐藏（与消耗段同纪律：不显示 0 tok/s 骗人）。
        m_rate = new QLabel(this);
        m_rate->setObjectName(QStringLiteral("ctxRate"));
        m_rate->setVisible(false);

        h->addWidget(m_numbers);
        h->addWidget(m_consumption);
        h->addWidget(m_rate);
    }

    /// 三段是否都空着——属主据此把本行连同分隔线一起隐藏（全空的计量行没有信息量）。
    /// 必须用 isHidden()（控件自己的显隐意愿）而不是 isVisible()（含祖先被隐藏的传导态）：
    /// 本行一旦被整条收起，三个标签的 isVisible() 会因父行隐藏而恒为 false，用后者判空就
    /// 把「父行隐藏」当成了「三段都没账」，第一个读数到达时也再展不开——整条永久消失。
    bool isEmpty() const {
        return m_numbers->isHidden() && m_consumption->isHidden() && m_rate->isHidden();
    }

    /// 占用数字段；空串即整段隐藏（与消耗/速度同纪律）
    void setNumbers(const QString& text, const QString& toolTip) {
        m_numbers->setToolTip(toolTip);
        m_numbers->setVisible(!text.isEmpty());
        setText(m_numbers, text);
    }

    /// 消耗段；空串即整段隐藏（还没收到任何 usage 回读时不显示「0」骗人）
    void setConsumption(const QString& text, const QString& toolTip) {
        m_consumption->setToolTip(toolTip);
        m_consumption->setVisible(!text.isEmpty());
        setText(m_consumption, text);
    }

    /// 速度段；空串即整段隐藏（还没拿到任何可定窗的请求时不显示「0 tok/s」骗人）
    void setRate(const QString& text, const QString& toolTip) {
        m_rate->setToolTip(toolTip);
        m_rate->setVisible(!text.isEmpty());
        setText(m_rate, text);
    }

protected:
    void resizeEvent(QResizeEvent*) override { relayoutTexts(); }

private:
    static constexpr int kSpacing = 10;
    static constexpr int kMinPart = 40;   // 数据段压缩下限：再窄就只剩省略号，不如保住可读片段

    /// 段全文缓存（省略后仍要能还原，且 tooltip 与全文是两回事——消耗/速度的
    /// tooltip 是口径说明，不能当全文用）
    QHash<QLabel*, QString> m_full;

    QVector<QLabel*> parts() const {
        QVector<QLabel*> visible;
        if (m_numbers->isVisible()) visible << m_numbers;
        if (m_consumption->isVisible()) visible << m_consumption;
        if (m_rate->isVisible()) visible << m_rate;
        return visible;
    }

    void setText(QLabel* label, const QString& full) {
        m_full.insert(label, full);
        applyText(label);
    }

    void applyText(QLabel* label) {
        const QString full = m_full.value(label);
        const int natural = label->fontMetrics().horizontalAdvance(full);
        const int share = m_share.value(label, natural);
        label->setText(share >= natural ? full
                                        : label->fontMetrics().elidedText(full, Qt::ElideRight, qMax(share, 1)));
    }

    /// 一行装不下时按各段自然宽比例压缩（等比缩，视觉上比「先砍尾段」稳）
    void relayoutTexts() {
        const QVector<QLabel*> visible = parts();
        if (visible.isEmpty()) return;

        if (width() <= 0) {                       // 未布局：先按全文占位，首帧布局后重算
            for (QLabel* label : visible) m_share.insert(label, label->fontMetrics().horizontalAdvance(m_full.value(label)));
        } else {
            const int avail = width() - kSpacing * int(visible.size());
            int sumNatural = 0;
            for (QLabel* label : visible) sumNatural += label->fontMetrics().horizontalAdvance(m_full.value(label));
            for (QLabel* label : visible) {
                const int natural = label->fontMetrics().horizontalAdvance(m_full.value(label));
                m_share.insert(label, sumNatural <= avail
                                          ? natural
                                          : qMax(kMinPart, int(double(avail) * double(natural) / double(sumNatural))));
            }
        }
        for (QLabel* label : visible) applyText(label);
    }

    QLabel* m_numbers = nullptr;
    QLabel* m_consumption = nullptr;
    QLabel* m_rate = nullptr;
    QHash<QLabel*, int> m_share;
};

// ============================================================================
// ChatStatusStrip
// ============================================================================

ChatStatusStrip::ChatStatusStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("chatStatusStrip"));
    // 自定义 QWidget 子类默认不渲染 QSS 盒模型（背景/边框），必须显式开启
    setAttribute(Qt::WA_StyledBackground, true);

    m_stripLayout = new QVBoxLayout(this);
    m_stripLayout->setContentsMargins(0, 0, 0, 0);
    m_stripLayout->setSpacing(6);

    // 与输入框之间的 1px 分隔线（用实体小条而非 border：QSS 单侧 border 画在控件
    // 矩形内侧，会把整条内容右移 1px，与侧栏 #sbSeparator 同处理）
    m_separator = new QWidget(this);
    m_separator->setObjectName(QStringLiteral("stripSeparator"));
    m_separator->setFixedHeight(1);
    m_stripLayout->addWidget(m_separator);

    m_stripRow = new StripRow(this);
    m_stripLayout->addWidget(m_stripRow);

    // 三段全空（会话刚建、一次 usage 都没回读）→ 整条隐藏，输入框下不留空线
    applyEmptiness();

    // 主题挂接惯例：构造函数末尾 bind（本组件无按主题着色的图标，无需 extraRefresh）
    ThemeAware::bind(QStringLiteral("ChatStatusStrip.qss"), this);
}

void ChatStatusStrip::setContextUsage(qsizetype usedTokens, qsizetype limitTokens) {
    const double ratio = UsageTokens::usageRatio(usedTokens, limitTokens);
    const QLocale loc(QLocale::c());
    const QString text = tr("≈%1 / %2 token · %3%")
                             .arg(loc.toString(usedTokens),
                                  loc.toString(limitTokens),
                                  QString::number(ratio * 100.0, 'f', 1));
    // tooltip 保留全文口径：本行只说「多大」，不说分母是派生预算（contextCharLimit/4），
    // 也不说估算与压缩触发同源——这两句决定数字能不能用来判断会不会被压缩改写。
    const QString tip = tr("上下文占用：≈%1 token 估算 ÷ %2 token 预算（预算是 settings.ini "
                           "字符串上限的派生值 /4，与压缩触发同源，会被后续回读覆盖）。")
                            .arg(loc.toString(usedTokens), loc.toString(limitTokens));
    m_stripRow->setNumbers(text, tip);
    applyEmptiness();   // 占用数字通常是第一个到账的读数：它出现即整条回显
}

void ChatStatusStrip::setTokenUsage(const UsageTokens::Totals &totals) {
    const QLocale loc(QLocale::c());
    if (totals.isEmpty()) {
        // 一次有效回读都没有：整段隐藏。显示「0 token」会被读成「没花钱」，
        // 而真相是「还没拿到数据」——两者对成本判断的作用相反，宁可不显示。
        m_stripRow->setConsumption(QString(), QString());
        applyEmptiness();
        return;
    }
    const QString text = tr("已用 %1 / 产出 %2 · 缓存命中 %3%")
                             .arg(loc.toString(totals.prompt),
                                  loc.toString(totals.completion),
                                  QString::number(totals.hitRatio() * 100.0, 'f', 1));
    // tooltip 逐条摊开明细 + 口径边界。边界必须写：本累计只覆盖带 include_usage 的
    // ChatStream 路径（主循环 + 队友回合），压缩摘要/记忆链/workflow runner 走
    // AsyncRequest（该类型无 usage 通道），完全不在账上。不写这句，用户会把它当会话总开销。
    const QString tip = tr("本会话累计 token 消耗（只增不减，与占用条口径无关）。\n"
                           "输入 %1 token，其中缓存命中 %2（未命中 %3）。\n"
                           "产出 %4 token，其中思考 %5。\n"
                           "有效回读 %6 次。\n"
                           "口径边界：仅统计主循环与队友回合；压缩摘要 / 记忆链 / "
                           "workflow runner 走 AsyncRequest 无 usage 回传，未计入本数。")
                            .arg(loc.toString(totals.prompt),
                                 loc.toString(totals.cached),
                                 loc.toString(totals.uncachedPrompt()),
                                 loc.toString(totals.completion),
                                 loc.toString(totals.reasoning),
                                 loc.toString(totals.calls));
    m_stripRow->setConsumption(text, tip);
    applyEmptiness();
}

void ChatStatusStrip::setOutputRate(const StreamRate::Meter &meter) {
    const QLocale loc(QLocale::c());
    if (meter.isEmpty()) {
        // 一个有效窗口都没有：整段隐藏。显示「0 tok/s」会被读成「模型很慢」，而真相是
        // 「还没拿到能定窗的数据」（无 usage 回读，或本轮是纯 tool_calls delta）——两者相反。
        m_stripRow->setRate(QString(), QString());
        applyEmptiness();
        return;
    }
    const QString text = tr("输出速度 %1 tok/s · 平均 %2 tok/s")
                             .arg(QString::number(meter.lastRate(), 'f', 1),
                                  QString::number(meter.averageRate(), 'f', 1));
    // tooltip 必须把窗口口径写全：行内数字只说「多少 tok/s」，不说分母是什么。分母不同，
    // 同一个数字的含义相反——把整请求窗口（含首字延迟与重试退避）当分母，慢首字会被读成慢模型。
    // 归属也要写清：last 未必属于本轮。纯 tool_calls 回合没有增量起点、无法定窗，此时 last
    // 仍是上一个可定窗——沿用「本轮」措辞会把上一轮的速率读成本轮的（与不显示 0 骗人同一条纪律）。
    const QString scope = meter.lastIsCurrent ? tr("本轮") : tr("上一可定窗");
    const QString caveat = meter.lastIsCurrent ? QString()
                                                : tr("（本回合无法定窗，显示的是上一个可定窗）");
    const QString tip = tr("按生成窗口计算：首个增量 → usage 末帧，不含首字延迟与工具执行时间。\n"
                           "%1 %2 token ÷ %3 ms = %4 tok/s%5。\n"
                           "平均 = 总产出 token ÷ 总生成时间（不是各轮速率的算术平均）。\n"
                           "首字延迟平均 %6 ms（含流内重试退避）。\n"
                           "口径边界：仅主循环请求；队友回合与压缩 / 记忆 / workflow 侧链不计时。"
                           "纯 tool_calls 回合无增量起点，无法定窗，不计入本数。")
                            .arg(scope,
                                 loc.toString(meter.last.tokens),
                                 loc.toString(meter.last.generationMs),
                                 QString::number(meter.lastRate(), 'f', 1),
                                 caveat)
                            .arg(QString::number(meter.averageFirstTokenMs(), 'f', 0));
    m_stripRow->setRate(text, tip);
    applyEmptiness();
}

void ChatStatusStrip::applyEmptiness() {
    // 空账纪律的收尾：三段各自隐藏由 StripRow 内部管，本函数只管「全空时连分隔线一起收」。
    // 会话刚建、一次 usage 都没回读时三段全空，此时输入框下只剩一条横线会被读成
    // 「状态条坏了」——整条隐藏，等第一个读数到达再回显（占用数字是常态首段）。
    const bool empty = m_stripRow->isEmpty();
    m_separator->setVisible(!empty);
    m_stripRow->setVisible(!empty);
}
