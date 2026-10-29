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

/// 计量文字行：**单行**——轮步段 + 消耗段 + 速度段。
/// 状态灯不在本行：三态灯整盏归会话侧栏「状态」节（见 SessionSidebar.h），本行只放
/// 「侧栏收起也必须常驻」的计量读数——两处同时亮灯会在同一时刻给出两个读数。
/// 占用**条**与占用数字也不在本行（同归侧栏「上下文」节；数字随条迁入面板是用户裁决，
/// 侧栏收起时随条一并不可见）；本行只承载文字，一行装不下时
/// 按各段自然宽比例压缩省略（tooltip 保留全文）。
/// 三段各自空账即整段隐藏（不显示 0 骗人）；三段全空时由属主 ChatStatusStrip 把
/// 本行整条隐藏，避免输入框下杵着一条没有内容的空白行。
/// 轮步段排在**最左**：它是「现在在干什么」的进行态定位，读序上先于「花了多少」与
/// 「吐字多快」；且它是最短的段（约 14 字），窄栏压缩时把最长的消耗段先砍更稳。
class StripRow : public QWidget {
public:
    explicit StripRow(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("stripRow"));
        setFixedHeight(26);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(kSpacing);
        // 轮步段（最左）：回答「现在在第几轮、本回合走到第几步」——进行态定位。
        // 与消耗/速度段的**空账判据不同**：那两段无数据即隐藏，本段 turns<=0 才隐藏，
        // 而 steps==0 是合法读数（本回合没调用工具就答完），照实显示「第 0 步」不隐藏
        // （隐藏会让「第 3 轮」孤零零出现并被读成「第 3 轮什么都没干」，比显示 0 更骗人）。
        m_turnStep = new QLabel(this);
        m_turnStep->setObjectName(QStringLiteral("ctxTurnStep"));
        m_turnStep->setVisible(false);

        // 消耗段：与占用**语义正交**（占用=窗口现在多大、会被后续回读覆盖，读数在侧栏
        // 「上下文」节；消耗=已经花掉多少、只增不减）。
        // 初值即隐藏：三段全空时 isEmpty() 才为真，属主据此把整条收起；
        // 若这里留一个「可见但无文字」的标签，空状态条会算成「有内容」而常驻一条空线
        m_consumption = new QLabel(this);
        m_consumption->setObjectName(QStringLiteral("ctxConsumption"));
        m_consumption->setVisible(false);
        // 速度段：与消耗段同层级、同色阶，但语义正交——消耗回答「已经花掉多少」，
        // 速度回答「模型吐字有多快」。空账整段隐藏（与消耗段同纪律：不显示 0 tok/s 骗人）。
        m_rate = new QLabel(this);
        m_rate->setObjectName(QStringLiteral("ctxRate"));
        m_rate->setVisible(false);

        h->addWidget(m_turnStep);   // 最左：进行态定位先于成本读数
        h->addWidget(m_consumption);
        h->addWidget(m_rate);
    }

    /// 三段是否都空着——属主据此把本行整条隐藏（全空的计量行没有信息量）。
    /// 必须用 isHidden()（控件自己的显隐意愿）而不是 isVisible()（含祖先被隐藏的传导态）：
    /// 本行一旦被整条收起，各标签的 isVisible() 会因父行隐藏而恒为 false，用后者判空就
    /// 把「父行隐藏」当成了「三段都没账」，第一个读数到达时也再展不开——整条永久消失。
    /// 【新增段必须同步本函数】它硬编码逐段列举，漏一段就等于那段不参与判空。
    bool isEmpty() const {
        return m_turnStep->isHidden() && m_consumption->isHidden() && m_rate->isHidden();
    }

    /// 轮步段；空串即整段隐藏（会话一个回合都没开过时不显示「第 0 轮 · 第 0 步」骗人）
    void setTurnStep(const QString& text, const QString& toolTip) {
        m_turnStep->setToolTip(toolTip);
        m_turnStep->setVisible(!text.isEmpty());
        setText(m_turnStep, text);
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
        if (m_turnStep->isVisible()) visible << m_turnStep;
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

    QLabel* m_turnStep = nullptr;
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

    m_stripRow = new StripRow(this);
    m_stripLayout->addWidget(m_stripRow);

    // 三段全空（会话刚建、一个回合都没开过、一次 usage 都没回读）→ 整条隐藏，输入框下不留空行
    applyEmptiness();

    // 主题挂接惯例：构造函数末尾 bind（本组件无按主题着色的图标，无需 extraRefresh）
    ThemeAware::bind(QStringLiteral("ChatStatusStrip.qss"), this);
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

void ChatStatusStrip::setTurnStep(int turns, int steps, int stepLimit) {
    // 空账判据与另两段不同：消耗/速度看「有没有拿到回读数据」，本段看「开过回合没有」。
    // turns<=0 = 会话从没跑过任何回合，此时「第 0 轮 · 第 0 步」是没账而非读数，整段隐藏。
    // steps 不做同判：0 是合法读数（本回合一次工具都没调就直接答完），照实显示。
    if (turns <= 0) {
        m_stripRow->setTurnStep(QString(), QString());
        applyEmptiness();
        return;
    }
    const QString text = tr("第 %1 轮 · 第 %2 步")
                             .arg(turns)
                             .arg(qMax(steps, 0));
    // 【术语锚定，本段 tooltip 的头号职责】本仓「轮」在设置页与引擎报错里指**工具调用
    // 轮次**（「工具调用轮次超过上限（%1 轮）」、「单轮最大调用次数」），侧栏 goal 节的
    // 「轮次 %1」又是 **goal 续跑计数**——三处不同义。本段的「轮」取的是第三种含义=
    // **用户回合**，与那两处都相反；不写这句，用户会拿本段的「轮」去对设置页的上限数，
    // 得出「第 3 轮怎么才 20 步」这类错误困惑。步=本回合内的工具调用轮次，才是与设置页
    // 同义的那个数。两个数各自的上限/归零时机也要写：轮只增不减（重启恢复按历史 user
    // 条数播种，压缩改写历史后可能偏小），步每回合归零。
    const QString tip = tr("进行态定位：本会话第 %1 个回合、该回合内第 %2 轮工具调用。\n"
                           "【术语提醒】本段的「轮」= 用户回合（一次发言开一个回合），"
                           "与设置页「单轮最大调用次数」和引擎报错「工具调用轮次超过上限」"
                           "里的「轮」**不同义**——那两处指的是下面这个「步」的数。\n"
                           "回合数含自动开回合（定时任务 / 团队事件 / 后台任务唤醒），"
                           "不是纯人工输入次数。\n"
                           "步数每回合归零，上限取设置页的「单轮最大调用次数」（%3），"
                           "撞上限即终止循环并报错。\n"
                           "重启恢复后回合数按历史里 user 消息条数播种；历史被压缩改写过时"
                           "该数会偏小（只反映压缩窗口内的存量）。")
                            .arg(turns)
                            .arg(qMax(steps, 0))
                            .arg(stepLimit);
    m_stripRow->setTurnStep(text, tip);
    applyEmptiness();
}

void ChatStatusStrip::applyEmptiness() {
    // 空账纪律的收尾：三段各自隐藏由 StripRow 内部管，本函数只管「全空时整行收起」。
    // 会话刚建、一个回合都没开过、一次 usage 都没回读时三段全空，此时输入框下会多出
    // 一行空白会被读成「状态条坏了」——整条隐藏，等第一个读数到达再回显。
    // 三段到账时机**不同批**（与拆出前的两段时代不同）：轮步段随首个回合开账即到账
    // （run() 入口），消耗/速度段要等第一次 usage 回读——所以常态下整条最先出现的
    // 是轮步段，这不是故障而是「引擎刚开工、账还没回来」的真实形态。
    const bool empty = m_stripRow->isEmpty();
    m_stripRow->setVisible(!empty);
}
