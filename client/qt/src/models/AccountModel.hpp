#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace pulse::client
{

/// Account balances from `get_account`, flattened into scalar properties.
///
/// The engine includes each block only when the corresponding exchange call
/// succeeds, so a missing block means "unknown", never "zero". Each presence
/// flag therefore has to be checked by the UI before rendering the value.
class AccountModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool futuresPresent READ futuresPresent NOTIFY changed)
    Q_PROPERTY(QString futuresCurrency READ futuresCurrency NOTIFY changed)
    Q_PROPERTY(double futuresTotal READ futuresTotal NOTIFY changed)
    Q_PROPERTY(double futuresAvailable READ futuresAvailable NOTIFY changed)
    Q_PROPERTY(double futuresUnrealised READ futuresUnrealised NOTIFY changed)
    Q_PROPERTY(double futuresPositionMargin READ futuresPositionMargin NOTIFY changed)
    Q_PROPERTY(double futuresOrderMargin READ futuresOrderMargin NOTIFY changed)
    Q_PROPERTY(bool spotPresent READ spotPresent NOTIFY changed)
    Q_PROPERTY(QString spotCurrency READ spotCurrency NOTIFY changed)
    Q_PROPERTY(double spotTotal READ spotTotal NOTIFY changed)
    Q_PROPERTY(double spotAvailable READ spotAvailable NOTIFY changed)
    Q_PROPERTY(bool cfdPresent READ cfdPresent NOTIFY changed)
    Q_PROPERTY(QString cfdCurrency READ cfdCurrency NOTIFY changed)
    Q_PROPERTY(double cfdTotal READ cfdTotal NOTIFY changed)
    Q_PROPERTY(double cfdAvailable READ cfdAvailable NOTIFY changed)
    Q_PROPERTY(double cfdMargin READ cfdMargin NOTIFY changed)
    Q_PROPERTY(double cfdUnrealised READ cfdUnrealised NOTIFY changed)

    /// Every non-zero spot currency, sorted by value descending. Each element
    /// is a map with currency / available / locked / total / valueUsdt /
    /// hasValue. `hasValue` is false when no ticker was cached, in which case
    /// valueUsdt is 0 and the UI must render a dash instead.
    Q_PROPERTY(QVariantList spotBalances READ spotBalances NOTIFY changed)

public:
    explicit AccountModel(QObject* parent = nullptr);

    /// Replace all values from a `get_account` result. Absent blocks clear
    /// their presence flag rather than leaving stale numbers on screen.
    void setFromJson(const QJsonObject& account);

    [[nodiscard]] bool futuresPresent() const { return m_futuresPresent; }
    [[nodiscard]] QString futuresCurrency() const { return m_futuresCurrency; }
    [[nodiscard]] double futuresTotal() const { return m_futuresTotal; }
    [[nodiscard]] double futuresAvailable() const { return m_futuresAvailable; }
    [[nodiscard]] double futuresUnrealised() const { return m_futuresUnrealised; }
    [[nodiscard]] double futuresPositionMargin() const { return m_futuresPositionMargin; }
    [[nodiscard]] double futuresOrderMargin() const { return m_futuresOrderMargin; }
    [[nodiscard]] bool spotPresent() const { return m_spotPresent; }
    [[nodiscard]] QString spotCurrency() const { return m_spotCurrency; }
    [[nodiscard]] double spotTotal() const { return m_spotTotal; }
    [[nodiscard]] double spotAvailable() const { return m_spotAvailable; }
    [[nodiscard]] bool cfdPresent() const { return m_cfdPresent; }
    [[nodiscard]] QString cfdCurrency() const { return m_cfdCurrency; }
    [[nodiscard]] double cfdTotal() const { return m_cfdTotal; }
    [[nodiscard]] double cfdAvailable() const { return m_cfdAvailable; }
    [[nodiscard]] double cfdMargin() const { return m_cfdMargin; }
    [[nodiscard]] double cfdUnrealised() const { return m_cfdUnrealised; }
    [[nodiscard]] QVariantList spotBalances() const { return m_spotBalances; }

signals:
    void changed();

private:
    bool m_futuresPresent = false;
    QString m_futuresCurrency;
    double m_futuresTotal = 0.0;
    double m_futuresAvailable = 0.0;
    double m_futuresUnrealised = 0.0;
    double m_futuresPositionMargin = 0.0;
    double m_futuresOrderMargin = 0.0;
    bool m_spotPresent = false;
    QString m_spotCurrency;
    double m_spotTotal = 0.0;
    double m_spotAvailable = 0.0;
    bool m_cfdPresent = false;
    QString m_cfdCurrency;
    double m_cfdTotal = 0.0;
    double m_cfdAvailable = 0.0;
    double m_cfdMargin = 0.0;
    double m_cfdUnrealised = 0.0;
    QVariantList m_spotBalances;
};

} // namespace pulse::client
