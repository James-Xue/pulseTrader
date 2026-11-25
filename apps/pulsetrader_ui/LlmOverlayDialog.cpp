// llm_overlay_dialog.cpp — non-modal Claude Code overlay viewer implementation.

#include "LlmOverlayDialog.hpp"

#include "ExternalSignalStore.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace pulse::ui
{

namespace
{

QString levelText(const QJsonValue &value)
{
    if (false == value.isArray())
    {
        return QStringLiteral("—");
    }
    QStringList parts;
    for (const QJsonValue &item : value.toArray())
    {
        if (item.isDouble())
        {
            parts << QString::number(item.toDouble(), 'f', 2);
        }
    }
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QLatin1Char('/'));
}

QString jsonTime(const QJsonValue &value)
{
    const QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    return parsed.isValid()
        ? parsed.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss"))
        : QStringLiteral("—");
}

QString shortJsonString(const QJsonValue &value, int maxLen = 1200)
{
    QString text = value.toString().simplified();
    if (text.size() > maxLen)
    {
        text = text.left(maxLen) + QStringLiteral("…");
    }
    return text.isEmpty() ? QStringLiteral("—") : text;
}

} // namespace

LlmOverlayDialog::LlmOverlayDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("LLM盘面"));
    setModal(false);
    resize(760, 540);

    auto *layout = new QVBoxLayout(this);

    m_status = new QLabel(QStringLiteral("暂无 LLM 盘面数据"), this);
    m_status->setStyleSheet(QStringLiteral(
        "font-size: 15px; font-weight: 600; color: #B26A00;"));
    layout->addWidget(m_status);

    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: #C0392B;"));
    m_error->setVisible(false);
    layout->addWidget(m_error);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const auto addField = [this, form](const QString &caption) {
        auto *value = new QLabel(QStringLiteral("—"), this);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(caption, value);
        return value;
    };
    m_time = addField(QStringLiteral("生成 / 有效期"));
    m_bias = addField(QStringLiteral("方向 / 置信度"));
    m_regime = addField(QStringLiteral("行情状态"));
    m_support = addField(QStringLiteral("支撑"));
    m_resistance = addField(QStringLiteral("阻力"));
    m_entry = addField(QStringLiteral("入场区"));
    m_stop = addField(QStringLiteral("参考止损"));
    m_targets = addField(QStringLiteral("参考目标"));
    m_risk = addField(QStringLiteral("风险标记"));
    m_context = addField(QStringLiteral("当前上下文"));
    m_reason = addField(QStringLiteral("理由"));
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout();
    auto *refresh = new QPushButton(QStringLiteral("刷新"), this);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    connect(refresh, &QPushButton::clicked, this, &LlmOverlayDialog::reload);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch();
    buttons->addWidget(refresh);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    m_timer = new QTimer(this);
    m_timer->setInterval(5'000);
    connect(m_timer, &QTimer::timeout, this, &LlmOverlayDialog::reload);
    m_timer->start();
    reload();
}

QString LlmOverlayDialog::auditPath() const
{
    return QFileInfo(ExternalSignalStore::statePath()).absolutePath()
        + QStringLiteral("/llm_overlay_audit.jsonl");
}

void LlmOverlayDialog::reload()
{
    const QString path = auditPath();
    QFile file(path);
    QJsonObject audit;
    if (file.open(QIODevice::ReadOnly))
    {
        const qint64 size = file.size();
        const qint64 start = size > 131072 ? size - 131072 : 0;
        file.seek(start);
        const QByteArray tail = file.readAll();
        file.close();
        const QList<QByteArray> lines = tail.split('\n');
        for (auto it = lines.crbegin(); it != lines.crend(); ++it)
        {
            if (it->trimmed().isEmpty())
            {
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(*it, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject())
            {
                audit = doc.object();
                break;
            }
        }
    }

    if (audit.isEmpty())
    {
        m_status->setText(QStringLiteral("暂无 LLM 盘面数据"));
        m_status->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: #B26A00;"));
        m_error->setText(QStringLiteral(
            "尚未收到 Claude Code overlay 审计记录。"
            "请确认 /xau-llm-overlay 循环正在运行。"));
        m_error->setVisible(true);
        for (QLabel *label : {m_time, m_bias, m_regime, m_support, m_resistance,
                              m_entry, m_stop, m_targets, m_risk, m_context,
                              m_reason})
        {
            label->setText(QStringLiteral("—"));
        }
        return;
    }

    const QString status = audit.value(QStringLiteral("status")).toString();
    QString statusText;
    if (status == QStringLiteral("ok"))
    {
        statusText = QStringLiteral("有效");
        m_status->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: #1E8E3E;"));
    }
    else if (status == QStringLiteral("missing"))
    {
        statusText = QStringLiteral("缺失");
        m_status->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: #C0392B;"));
    }
    else if (status == QStringLiteral("stale"))
    {
        statusText = QStringLiteral("已过期");
        m_status->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: #C0392B;"));
    }
    else
    {
        statusText = QStringLiteral("格式错误");
        m_status->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: #C0392B;"));
    }
    m_status->setText(QStringLiteral("LLM盘面状态：%1（%2）")
                          .arg(statusText,
                               audit.value(QStringLiteral("at")).toString()));

    const QString error = audit.value(QStringLiteral("error")).toString();
    m_error->setText(error);
    m_error->setVisible(false == error.isEmpty());

    const QJsonObject overlay =
        audit.value(QStringLiteral("overlay")).toObject();
    const QString bias = overlay.value(QStringLiteral("bias")).toString().toLower();
    const QString biasCn = (bias == QStringLiteral("long"))
        ? QStringLiteral("多")
        : (bias == QStringLiteral("short") ? QStringLiteral("空")
                                          : QStringLiteral("中性"));
    const QString regime =
        overlay.value(QStringLiteral("regime")).toString().toLower();
    const QString regimeCn = (regime == QStringLiteral("trend"))
        ? QStringLiteral("趋势")
        : (regime == QStringLiteral("range") ? QStringLiteral("震荡")
                                             : QStringLiteral("过渡"));

    m_time->setText(QStringLiteral("%1 → %2")
                        .arg(jsonTime(overlay.value(QStringLiteral("generated_at"))),
                             jsonTime(overlay.value(QStringLiteral("valid_until")))));
    m_bias->setText(QStringLiteral("%1 / %2")
                        .arg(biasCn,
                             QString::number(
                                 overlay.value(QStringLiteral("confidence"))
                                     .toDouble(),
                                 'f', 2)));
    m_regime->setText(regimeCn);
    m_support->setText(levelText(overlay.value(QStringLiteral("support"))));
    m_resistance->setText(levelText(overlay.value(QStringLiteral("resistance"))));
    m_entry->setText(levelText(overlay.value(QStringLiteral("entry_zone"))));
    m_stop->setText(overlay.value(QStringLiteral("stop")).isDouble()
                        ? QString::number(
                              overlay.value(QStringLiteral("stop")).toDouble(),
                              'f', 2)
                        : QStringLiteral("—"));
    m_targets->setText(levelText(overlay.value(QStringLiteral("targets"))));

    QStringList flags;
    for (const QJsonValue &item :
         overlay.value(QStringLiteral("risk_flags")).toArray())
    {
        if (item.isString())
        {
            flags << item.toString();
        }
    }
    m_risk->setText(flags.isEmpty() ? QStringLiteral("—")
                                    : flags.join(QLatin1Char('/')));

    const double price = audit.value(QStringLiteral("last_price")).toDouble();
    const double balance = audit.value(QStringLiteral("balance_usdt")).toDouble();
    const int positions = audit.value(QStringLiteral("positions")).toInt();

    // Prefer the live external_state.json for the context line. The audit
    // record can be written a few seconds before the first ticker updates
    // m_lastPrice in the main window.
    QJsonObject state;
    QFile stateFile(ExternalSignalStore::statePath());
    if (stateFile.open(QIODevice::ReadOnly))
    {
        const QJsonDocument doc = QJsonDocument::fromJson(stateFile.readAll());
        stateFile.close();
        if (doc.isObject())
        {
            state = doc.object();
        }
    }
    const double statePrice = state.value(QStringLiteral("last_price")).toDouble();
    const double stateBalance =
        state.value(QStringLiteral("balance_usdt")).toDouble();
    const QJsonArray statePositions =
        state.value(QStringLiteral("positions")).toArray();
    const double shownPrice = statePrice > 0.0 ? statePrice : price;
    const double shownBalance = stateBalance > 0.0 ? stateBalance : balance;
    const int shownPositions = state.isEmpty()
        ? positions
        : static_cast<int>(statePositions.size());
    QString positionSide;
    double positionEntry = 0.0;
    if (false == statePositions.isEmpty()
        && statePositions.first().isObject())
    {
        const QJsonObject position = statePositions.first().toObject();
        positionSide = position.value(QStringLiteral("side")).toString();
        positionEntry =
            position.value(QStringLiteral("entry_price")).toDouble();
    }
    else
    {
        positionSide = audit.value(QStringLiteral("position_side")).toString();
        positionEntry =
            audit.value(QStringLiteral("position_entry")).toDouble();
    }

    QString context = QStringLiteral("现价 %1")
                          .arg(shownPrice > 0.0
                                   ? QString::number(shownPrice, 'f', 2)
                                   : QStringLiteral("—"));
    if (shownBalance > 0.0)
    {
        context += QStringLiteral(" | 余额 %1U")
                       .arg(QString::number(shownBalance, 'f', 2));
    }
    context += QStringLiteral(" | %1")
                   .arg(shownPositions > 0
                            ? QStringLiteral("持仓 %1 笔").arg(shownPositions)
                            : QStringLiteral("空仓"));
    if (shownPositions > 0)
    {
        if (false == positionSide.isEmpty())
        {
            context += QStringLiteral("（%1 @%2）")
                           .arg(positionSide == QStringLiteral("short")
                                    ? QStringLiteral("空")
                                    : QStringLiteral("多"),
                                QString::number(positionEntry, 'f', 2));
        }
    }
    m_context->setText(context);
    m_reason->setText(shortJsonString(
        overlay.value(QStringLiteral("reason"))));
}

} // namespace pulse::ui
