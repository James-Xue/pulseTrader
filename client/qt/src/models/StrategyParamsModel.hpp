#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace pulse::client
{

/// Parameters of one selected strategy instance, from `get_strategy_params`.
///
/// The payload is a flat object of numeric values with no id echoed back, so
/// the caller must supply the instance id. That id is also the staleness
/// guard: a reply for an instance the user has already navigated away from is
/// ignored instead of overwriting the panel.
///
/// Emission model: exactly one of `pending`, `present` or `error` describes
/// the current state.
class StrategyParamsModel : public QObject
{
    Q_OBJECT

    /// The instance these values belong to (empty when cleared).
    Q_PROPERTY(QString strategyId READ strategyId NOTIFY changed)
    /// True only when values are available.
    Q_PROPERTY(bool present READ present NOTIFY changed)
    /// True while the request is in flight and no values are available yet.
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    /// RPC failure text (for example "strategy not found"); empty on success.
    Q_PROPERTY(QString error READ error NOTIFY changed)
    /// Ordered list of `{ key, value }` maps: the 14 engine parameters in
    /// canonical order, then any unknown keys alphabetically.
    Q_PROPERTY(QVariantList params READ params NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    explicit StrategyParamsModel(QObject* parent = nullptr);

    /// Mark a request for `strategyId` as in flight and clear the old values.
    void setPending(const QString& strategyId);

    /// Apply a successful reply. Ignored when `strategyId` is not the current
    /// selection, which is how a late reply for a previous instance is dropped.
    void setFromJson(const QString& strategyId, const QJsonObject& params);

    /// Record a failure for `strategyId`. Ignored when it is not the current
    /// selection, so an old failure cannot paint over a newer selection.
    void setError(const QString& strategyId, const QString& message);

    /// Clear everything (selection removed or connection torn down).
    void clear();

    [[nodiscard]] QString strategyId() const { return m_strategyId; }
    [[nodiscard]] bool present() const { return m_present; }
    [[nodiscard]] bool pending() const { return m_pending; }
    [[nodiscard]] QString error() const { return m_error; }
    [[nodiscard]] QVariantList params() const { return m_params; }
    [[nodiscard]] int count() const { return static_cast<int>(m_params.size()); }

signals:
    void changed();

private:
    /// Canonical display order. Parameters outside this list are appended in
    /// the order the JSON object iterates them (QJsonObject is key-sorted),
    /// so a new engine parameter still appears without a client release.
    [[nodiscard]] static const QStringList& canonicalKeys();

    QString m_strategyId;
    bool m_present = false;
    bool m_pending = false;
    QString m_error;
    QVariantList m_params;
};

} // namespace pulse::client