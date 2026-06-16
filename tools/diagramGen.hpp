#pragma once
// diagramGen.hpp — Header-only DiagramGenerator for pulseTrader
// Generates fixed-visual-width ASCII/Unicode box diagrams.
//
// All box-drawing characters (─ │ ┌ ┐ └ ┘ ┬ ├ ▼ ►) are treated as
// double-width (2 columns) in a CJK-locale monospace font.  Every other
// character (ASCII, space) is single-width (1 column).
//
// Target total visual width: configurable (default 80).
//
// C++20, header-only, no third-party dependencies.

#include <cassert>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pulse
{

// ---------------------------------------------------------------------------
// Unicode helpers (internal detail namespace)
// ---------------------------------------------------------------------------

namespace detail
{

/// Decode the first UTF-8 code point in [p, end).
///
/// Steps:
///   1. If the pointer is past the end, return 0 (end-of-input sentinel)
///   2. Read the lead byte; if ASCII (< 0x80), return it directly
///   3. Determine the expected byte count from the lead byte's high bits
///   4. Consume continuation bytes, accumulating the code point
///   5. Return U+FFFD on any invalid sequence
inline char32_t utf8Next(const char *&p, const char *end) noexcept
{
    if (p >= end)
    {
        return 0;
    }
    unsigned char c = static_cast<unsigned char>(*p);

    // ASCII fast path — single byte, no continuation
    if (c < 0x80)
    {
        ++p;
        return c;
    }

    int extra = 0;
    char32_t cp = 0;

    // Determine sequence length from the lead byte's high bits
    if (0xC0 == (c & 0xE0))
    {
        extra = 1;
        cp = c & 0x1F;
    }
    else if (0xE0 == (c & 0xF0))
    {
        extra = 2;
        cp = c & 0x0F;
    }
    else if (0xF0 == (c & 0xF8))
    {
        extra = 3;
        cp = c & 0x07;
    }
    else
    {
        ++p;
        return 0xFFFD; // invalid lead byte
    }

    ++p;

    // Consume continuation bytes (each must have top bits == 10xxxxxx)
    for (int i = 0; i < extra; ++i)
    {
        if (p >= end || 0x80 != (static_cast<unsigned char>(*p) & 0xC0))
        {
            return 0xFFFD; // truncated or invalid continuation
        }
        cp = (cp << 6) | (static_cast<unsigned char>(*p) & 0x3F);
        ++p;
    }
    return cp;
}

/// Return true for every box-drawing code point used in pulseTrader diagrams.
///
/// These code points are rendered as double-width (2 columns) in CJK fonts.
/// The explicit list matches the character table in highLevelArchitecture.md:
///   ─ U+2500   │ U+2502   ┌ U+250C   ┐ U+2510
///   └ U+2514   ┘ U+2518   ┬ U+252C   ├ U+251C
///   ▼ U+25BC   ► U+25BA
inline bool isDoubleWidth(char32_t cp) noexcept
{
    switch (cp)
    {
    case 0x2500: // ─
    case 0x2502: // │
    case 0x250C: // ┌
    case 0x2510: // ┐
    case 0x2514: // └
    case 0x2518: // ┘
    case 0x252C: // ┬
    case 0x251C: // ├
    case 0x25BC: // ▼
    case 0x25BA: // ►
        return true;
    default:
        return false;
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API — free functions
// ---------------------------------------------------------------------------

/// Compute the visual (terminal column) width of a UTF-8 string.
///
/// Steps:
///   1. Iterate over each UTF-8 code point in the string
///   2. Add 2 for double-width box-drawing characters, 1 for everything else
///   3. Return the accumulated visual width
inline int visualWidth(std::string_view s) noexcept
{
    int w = 0;
    const char *p = s.data();
    const char *end = p + s.size();
    while (p < end)
    {
        char32_t cp = detail::utf8Next(p, end);
        w += detail::isDoubleWidth(cp) ? 2 : 1;
    }
    return w;
}

/// Return a copy of s padded with trailing spaces so its visual width == targetVw.
/// If visualWidth(s) >= targetVw the string is returned unchanged.
inline std::string padToVisual(std::string_view s, int targetVw)
{
    int cur = visualWidth(s);
    std::string result(s);
    if (cur < targetVw)
    {
        result.append(static_cast<std::size_t>(targetVw - cur), ' ');
    }
    return result;
}

/// Generate a horizontal line string composed of N '─' (U+2500) characters.
///
/// Steps:
///   1. Reserve space for N × 3 bytes (each ─ is 3 UTF-8 bytes: E2 94 80)
///   2. Append the 3-byte sequence N times
///   3. Return the result (visual width = 2 × N)
inline std::string makeHline(int n)
{
    std::string result;
    result.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i)
    {
        result += "\xe2\x94\x80"; // ─ (U+2500, 3 UTF-8 bytes)
    }
    return result;
}

// ---------------------------------------------------------------------------
// DiagramGenerator
//
// Builds a box diagram with exact visual-width constraints.
// Default total visual width: 80 columns.
//
// Layout constants (for totalVw == 80):
//   outer frame  : ┌ + 38×─ + ┐                                    (80 vw)
//   outer content: │ + <76 vw> + │                                  (80 vw)
//   inner frame  : [2 sp] + ┌ + 33×─ + ┐ + [4 sp]  →  76 vw content
//   inner content: [2 sp] + │ + <68 vw> + │ + [2 sp] →  76 vw content
//   connector    : [37 sp] + │/▼ + [1 sp] + label + trailing       (80 vw)
//
// For other totalVw values the constants are computed proportionally.
// ---------------------------------------------------------------------------
class DiagramGenerator
{
  public:
    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /// @param totalVw  Target visual width for every output line (default 80).
    ///
    /// Steps:
    ///   1. Validate that totalVw >= 20 (minimum usable width)
    ///   2. Compute inner content width (totalVw minus two outer │ chars)
    ///   3. Compute outer hline count from total width
    ///   4. Compute inner hline count and trailing space count
    ///   5. Compute inner text width and connector prefix alignment
    explicit DiagramGenerator(int totalVw = 80) : totalVw_(totalVw)
    {
        if (totalVw < 20)
        {
            throw std::invalid_argument("totalVw must be at least 20");
        }

        // Outer │ chars each = 2 vw → inner content = totalVw - 4
        innerVw_ = totalVw_ - 4;

        // Outer hline count: ┌(2) + N×─(2) + ┐(2) = totalVw → N = (totalVw - 4) / 2
        outerHlineCount_ = (totalVw_ - 4) / 2;

        // Inner frame: "  ┌" + M×─ + "┐" + trailing_spaces = innerVw_
        //   2(spaces) + 2(┌) + 2M(─) + 2(┐) + trailing = innerVw_
        //   trailing = innerVw_ - 6 - 2M
        // Choose M so trailing >= 2:  M = (innerVw_ - 8) / 2
        innerHlineCount_ = (innerVw_ - 8) / 2;
        innerFrameTrailing_ = innerVw_ - 2 - 2 - innerHlineCount_ * 2 - 2;

        // Inner content text width:
        //   "  │" + text(T vw) + "│  " = innerVw_
        //   2 + 2 + T + 2 + 2 = innerVw_ → T = innerVw_ - 8
        innerTextVw_ = innerVw_ - 8;

        // Connector: spaces before │/▼ symbol
        //   Aligned at the visual center of the inner frame's horizontal line.
        //   Inner ┌ starts at vw offset 4 (2 spaces + 2 for ┌),
        //   spans innerHlineCount_ × 2 columns.  Center = 4 + innerHlineCount_.
        connectorPrefixSpaces_ = 2 + 2 + innerHlineCount_;
    }

    // -----------------------------------------------------------------------
    // Building blocks
    // -----------------------------------------------------------------------

    /// Begin the outer enclosing box; optionally print a centred title row.
    ///
    /// Steps:
    ///   1. Draw the top border (┌──…──┐)
    ///   2. If a title is provided, centre it within the inner content area
    ///   3. Add a blank separator row
    void outerBoxBegin(std::string_view title = "")
    {
        // Step 1: top border
        std::string top = "\xe2\x94\x8c"; // ┌
        top += makeHline(outerHlineCount_);
        top += "\xe2\x94\x90"; // ┐
        lines_.push_back(std::move(top));

        // Step 2: optional centred title
        if (!title.empty())
        {
            int tv = visualWidth(title);
            int lpad = (innerVw_ - tv) / 2;
            int rpad = innerVw_ - tv - lpad;
            std::string content(static_cast<std::size_t>(lpad), ' ');
            content += title;
            content.append(static_cast<std::size_t>(rpad), ' ');
            lines_.push_back(makeOuterRow(content));
        }

        // Step 3: blank separator
        lines_.push_back(makeBlankOuterRow());
    }

    /// End the outer enclosing box.
    void outerBoxEnd()
    {
        lines_.push_back(makeBlankOuterRow());

        std::string bot = "\xe2\x94\x94"; // └
        bot += makeHline(outerHlineCount_);
        bot += "\xe2\x94\x98"; // ┘
        lines_.push_back(std::move(bot));
    }

    /// Add an inner module box with 1–3 content lines.
    ///
    /// @param line1  First content line (layer header, required).
    /// @param line2  Second content line (optional, indented 4 spaces).
    /// @param line3  Third content line (optional, indented 4 spaces).
    void innerBox(std::string_view line1, std::string_view line2 = "", std::string_view line3 = "")
    {
        lines_.push_back(makeInnerTop());
        lines_.push_back(makeInnerRow("  " + std::string(line1)));
        if (!line2.empty())
        {
            lines_.push_back(makeInnerRow("    " + std::string(line2)));
        }
        if (!line3.empty())
        {
            lines_.push_back(makeInnerRow("    " + std::string(line3)));
        }
        lines_.push_back(makeInnerBottom());
    }

    /// Add a vertical connector line (│) with an optional label to the right.
    void connectorDown(std::string_view label = "")
    {
        lines_.push_back(makeConnector("\xe2\x94\x82", label)); // │
    }

    /// Add a downward arrow connector (▼) with an optional label to the right.
    void connectorArrow(std::string_view label = "")
    {
        lines_.push_back(makeConnector("\xe2\x96\xbc", label)); // ▼
    }

    /// Add a blank outer row (empty line inside the outer box).
    void blankLine()
    {
        lines_.push_back(makeBlankOuterRow());
    }

    // -----------------------------------------------------------------------
    // Render
    // -----------------------------------------------------------------------

    /// Return the complete diagram as a single string (lines separated by '\n').
    std::string render() const
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            if (0 < i)
            {
                oss << '\n';
            }
            oss << lines_[i];
        }
        return oss.str();
    }

    /// Return each line as a separate string.
    const std::vector<std::string> &lines() const noexcept
    {
        return lines_;
    }

    // -----------------------------------------------------------------------
    // Accessors (useful for unit tests)
    // -----------------------------------------------------------------------
    int totalVw() const noexcept
    {
        return totalVw_;
    }
    int innerVw() const noexcept
    {
        return innerVw_;
    }
    int innerTextVw() const noexcept
    {
        return innerTextVw_;
    }
    int connectorPrefix() const noexcept
    {
        return connectorPrefixSpaces_;
    }

  private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Wrap content (must have visual width == innerVw_) with outer │ chars.
    std::string makeOuterRow(std::string_view content) const
    {
        std::string row;
        row.reserve(content.size() + 6);
        row += "\xe2\x94\x82"; // │
        row += content;
        row += "\xe2\x94\x82"; // │
        return row;
    }

    /// Build outer row from content string, padding to innerVw_ first.
    std::string makeOuterRowPadded(std::string_view content) const
    {
        return makeOuterRow(padToVisual(content, innerVw_));
    }

    /// Build an outer row filled with spaces (blank line inside the box).
    std::string makeBlankOuterRow() const
    {
        return makeOuterRow(std::string(static_cast<std::size_t>(innerVw_), ' '));
    }

    /// Build the top border of an inner module box: "  ┌──…──┐    ".
    std::string makeInnerTop() const
    {
        std::string content = "  ";
        content += "\xe2\x94\x8c"; // ┌
        content += makeHline(innerHlineCount_);
        content += "\xe2\x94\x90"; // ┐
        content.append(static_cast<std::size_t>(innerFrameTrailing_), ' ');
        return makeOuterRow(content);
    }

    /// Build the bottom border of an inner module box: "  └──…──┘    ".
    std::string makeInnerBottom() const
    {
        std::string content = "  ";
        content += "\xe2\x94\x94"; // └
        content += makeHline(innerHlineCount_);
        content += "\xe2\x94\x98"; // ┘
        content.append(static_cast<std::size_t>(innerFrameTrailing_), ' ');
        return makeOuterRow(content);
    }

    /// Build an inner content row: "  │<text padded to innerTextVw_>│  ".
    std::string makeInnerRow(std::string_view text) const
    {
        std::string innerText = padToVisual(text, innerTextVw_);
        std::string content = "  ";
        content += "\xe2\x94\x82"; // │
        content += innerText;
        content += "\xe2\x94\x82"; // │
        content += "  ";
        return makeOuterRow(content);
    }

    /// Build a connector row: prefix spaces + symbol(2vw) + space + label + trailing.
    ///
    /// @param symUtf8  The connector symbol in UTF-8 (│ or ▼, each 2 vw).
    /// @param label    Optional text label to the right of the symbol.
    std::string makeConnector(std::string_view symUtf8, std::string_view label) const
    {
        std::string content(static_cast<std::size_t>(connectorPrefixSpaces_), ' ');
        content += symUtf8; // │ or ▼ (2 vw)
        content += ' ';
        content += label;
        int cur = visualWidth(content);
        int trailing = innerVw_ - cur;
        if (0 < trailing)
        {
            content.append(static_cast<std::size_t>(trailing), ' ');
        }
        return makeOuterRow(content);
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    int totalVw_;               ///< Target visual width for every output line.
    int innerVw_;               ///< Inner content width (totalVw - 4).
    int outerHlineCount_;       ///< Number of ─ chars in the outer top/bottom border.
    int innerHlineCount_;       ///< Number of ─ chars in the inner top/bottom border.
    int innerFrameTrailing_;    ///< Trailing spaces after inner ┐/┘ to fill innerVw_.
    int innerTextVw_;           ///< Visual width available for text inside inner boxes.
    int connectorPrefixSpaces_; ///< Leading spaces before the connector symbol.

    std::vector<std::string> lines_; ///< Accumulated output lines.
};

} // namespace pulse

// ---------------------------------------------------------------------------
// Usage example (compiled only when DIAGRAM_GEN_MAIN is defined)
// ---------------------------------------------------------------------------
#ifdef DIAGRAM_GEN_MAIN
#include <iostream>

int main()
{
    pulse::DiagramGenerator gen(80);

    gen.outerBoxBegin("pulseTrader Process");

    // --- Decision path (top-down control flow) ---
    gen.innerBox("Layer 5: HeartbeatScheduler  (every 5 min)", "└─► TaskQueue ──► AIAnalyzer (L4) ──► ParamAdvisor");
    gen.connectorArrow("param updates (atomic writes)");

    gen.innerBox("Layer 6: Strategy Engine",
        "MomentumScalper | OrderBookScalper | MeanReversionScalper",
        "SignalAggregator (weighted voting)");
    gen.connectorArrow("signals");

    gen.innerBox("Layer 7: Risk Management", "RiskManager | PositionManager | StopLoss/TakeProfit Engines");
    gen.connectorArrow("approved orders");

    gen.innerBox("Layer 8: Order Execution", "OrderExecutor | OrderTracker | ExecutionReport");
    gen.blankLine();

    // --- Data source (bottom-up feed) ---
    gen.innerBox("Layer 3: Market Data  (hot path -- dedicated thread)",
        "MarketFeed | OrderBookManager | KlineBuffer | TickerCache");
    gen.connectorArrow("raw frames");

    gen.innerBox(
        "Layer 1: Exchange  (Gate.io REST + WebSocket)", "GateRestClient | GateWsClient | GateWsChannels | GateAuth");
    gen.blankLine();

    // --- Cross-cutting infrastructure ---
    gen.innerBox(
        "Layer 2: Logging & Monitoring  (cross-cutting)", "Logger | TradeRecorder | MetricsCollector | AlertManager");

    gen.outerBoxEnd();

    std::cout << gen.render() << '\n';

    // Verify all lines have visual width == 80
    bool allOk = true;
    for (const auto &line : gen.lines())
    {
        int vw = pulse::visualWidth(line);
        if (80 != vw)
        {
            std::cerr << "FAIL vw=" << vw << " : " << line << '\n';
            allOk = false;
        }
    }
    if (allOk)
    {
        std::cerr << "OK — all " << gen.lines().size() << " lines have visual width 80\n";
    }

    return allOk ? 0 : 1;
}
#endif // DIAGRAM_GEN_MAIN
