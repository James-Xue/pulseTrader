#pragma once

#include <QJsonObject>
#include <QObject>

namespace pulse::client
{

/// Engine-wide risk snapshot from `get_risk`, flattened into scalars.
///
/// The wire object mixes snake_case and camelCase on purpose
/// (`trading_halted`, `haltReason`, `dailyDrawdown`, ...), so every key below
/// was taken from the engine's serializer rather than guessed. A missing
/// block means "the engine did not report it", never "zero": the `present`
/// and `portfolioPresent` flags exist so the UI can render `unknown` instead
/// of a fabricated 0.
class RiskModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool present READ present NOTIFY changed)
    Q_PROPERTY(bool tradingHalted READ tradingHalted NOTIFY changed)
    /// Numeric pulse::ErrorCode (0 = none). The page maps it to a label; this
    /// class deliberately does not link engine code just to spell an enum.
    Q_PROPERTY(int haltReasonCode READ haltReasonCode NOTIFY changed)
    /// Fraction of equity, not a percentage (0.05 = 5%).
    Q_PROPERTY(double dailyDrawdown READ dailyDrawdown NOTIFY changed)
    /// Fraction of equity, not a percentage.
    Q_PROPERTY(double maxDrawdown READ maxDrawdown NOTIFY changed)
    /// Raw token count. The engine publishes no capacity, so the UI must never
    /// render "x / y".
    Q_PROPERTY(double rateLimiterTokens READ rateLimiterTokens NOTIFY changed)
    Q_PROPERTY(bool rateLimiterExhausted READ rateLimiterExhausted NOTIFY changed)
    Q_PROPERTY(bool portfolioPresent READ portfolioPresent NOTIFY changed)
    Q_PROPERTY(int portfolioOpenPositions READ portfolioOpenPositions NOTIFY changed)
    Q_PROPERTY(double portfolioNotional READ portfolioNotional NOTIFY changed)
    Q_PROPERTY(double portfolioUnrealizedPnl READ portfolioUnrealizedPnl NOTIFY changed)
    Q_PROPERTY(double portfolioNetExposure READ portfolioNetExposure NOTIFY changed)
    Q_PROPERTY(int openPositionCount READ openPositionCount NOTIFY changed)

public:
    explicit RiskModel(QObject* parent = nullptr);

    /// Replace all values from a `get_risk` result. An empty object clears the
    /// presence flags so the UI renders `unknown` rather than zeroes.
    void setFromJson(const QJsonObject& risk);

    [[nodiscard]] bool present() const { return m_present; }
    [[nodiscard]] bool tradingHalted() const { return m_tradingHalted; }
    [[nodiscard]] int haltReasonCode() const { return m_haltReasonCode; }
    [[nodiscard]] double dailyDrawdown() const { return m_dailyDrawdown; }
    [[nodiscard]] double maxDrawdown() const { return m_maxDrawdown; }
    [[nodiscard]] double rateLimiterTokens() const { return m_rateLimiterTokens; }
    [[nodiscard]] bool rateLimiterExhausted() const { return m_rateLimiterExhausted; }
    [[nodiscard]] bool portfolioPresent() const { return m_portfolioPresent; }
    [[nodiscard]] int portfolioOpenPositions() const { return m_portfolioOpenPositions; }
    [[nodiscard]] double portfolioNotional() const { return m_portfolioNotional; }
    [[nodiscard]] double portfolioUnrealizedPnl() const { return m_portfolioUnrealizedPnl; }
    [[nodiscard]] double portfolioNetExposure() const { return m_portfolioNetExposure; }
    [[nodiscard]] int openPositionCount() const { return m_openPositionCount; }

signals:
    /// One signal for every property: the engine sends the snapshot as a
    /// whole, so per-field notifications would only add noise.
    void changed();

private:
    bool m_present = false;
    bool m_tradingHalted = false;
    int m_haltReasonCode = 0;
    double m_dailyDrawdown = 0.0;
    double m_maxDrawdown = 0.0;
    double m_rateLimiterTokens = 0.0;
    bool m_rateLimiterExhausted = false;
    bool m_portfolioPresent = false;
    int m_portfolioOpenPositions = 0;
    double m_portfolioNotional = 0.0;
    double m_portfolioUnrealizedPnl = 0.0;
    double m_portfolioNetExposure = 0.0;
    int m_openPositionCount = 0;
};

} // namespace pulse::client