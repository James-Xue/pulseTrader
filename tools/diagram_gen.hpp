#pragma once
// diagram_gen.hpp — Header-only DiagramGenerator for pulseTrader
// Generates fixed-visual-width ASCII/Unicode box diagrams.
//
// All box-drawing characters (─ │ ┌ ┐ └ ┘ ┬ ├ ▼ ►) are treated as
// double-width (2 columns) in a CJK-locale monospace font.  Every other
// character (ASCII, space) is single-width (1 column).
//
// Target total visual width: configurable (default 80).
//
// C++20, header-only, no third-party dependencies.

#include <string>
#include <string_view>
#include <vector>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <sstream>

namespace pulse
{

    // ---------------------------------------------------------------------------
    // Unicode helpers
    // ---------------------------------------------------------------------------

    namespace detail
    {

        /// Decode the first UTF-8 code point in [p, end).
        /// Returns the code point and advances *p past the bytes consumed.
        /// Returns U+FFFD and advances by 1 on invalid input.
        inline char32_t utf8_next(const char *&p, const char *end) noexcept
        {
            if (p >= end)
                return 0;
            unsigned char c = static_cast<unsigned char>(*p);

            if (c < 0x80)
            {
                ++p;
                return c;
            }

            int extra = 0;
            char32_t cp = 0;

            if ((c & 0xE0) == 0xC0)
            {
                extra = 1;
                cp = c & 0x1F;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                extra = 2;
                cp = c & 0x0F;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                extra = 3;
                cp = c & 0x07;
            }
            else
            {
                ++p;
                return 0xFFFD;
            } // invalid lead byte

            ++p;
            for (int i = 0; i < extra; ++i)
            {
                if (p >= end || (static_cast<unsigned char>(*p) & 0xC0) != 0x80)
                {
                    return 0xFFFD;
                }
                cp = (cp << 6) | (static_cast<unsigned char>(*p) & 0x3F);
                ++p;
            }
            return cp;
        }

        /// Return true for every box-drawing code point used in pulseTrader diagrams.
        /// These code points are rendered as double-width (2 columns) in CJK fonts.
        inline bool is_double_width(char32_t cp) noexcept
        {
            // Explicit list matching the character table in highLevelArchitecture.md:
            //   ─ U+2500   │ U+2502   ┌ U+250C   ┐ U+2510
            //   └ U+2514   ┘ U+2518   ┬ U+252C   ├ U+251C
            //   ▼ U+25BC   ► U+25BA
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
    /// Double-width box-drawing characters count as 2; everything else counts as 1.
    inline int visual_width(std::string_view s) noexcept
    {
        int w = 0;
        const char *p = s.data();
        const char *end = p + s.size();
        while (p < end)
        {
            char32_t cp = detail::utf8_next(p, end);
            w += detail::is_double_width(cp) ? 2 : 1;
        }
        return w;
    }

    /// Return a copy of s padded with trailing spaces so its visual width == target_vw.
    /// If visual_width(s) >= target_vw the string is returned unchanged.
    inline std::string pad_to_visual(std::string_view s, int target_vw)
    {
        int cur = visual_width(s);
        std::string result(s);
        if (cur < target_vw)
            result.append(static_cast<std::size_t>(target_vw - cur), ' ');
        return result;
    }

    /// Generate a horizontal line string composed of N '─' (U+2500) characters.
    /// Each character has visual width 2, so the result has visual width 2*n.
    inline std::string make_hline(int n)
    {
        // U+2500 ─ encodes as 3 UTF-8 bytes: E2 94 80
        std::string result;
        result.reserve(static_cast<std::size_t>(n) * 3);
        for (int i = 0; i < n; ++i)
            result += "\xe2\x94\x80"; // ─
        return result;
    }

    // ---------------------------------------------------------------------------
    // DiagramGenerator
    // ---------------------------------------------------------------------------

    /// Builds a box diagram with the exact visual-width constraints described in
    /// highLevelArchitecture.md.  Default total visual width: 80 columns.
    ///
    /// Layout constants (for total_vw == 80):
    ///   outer frame  : ┌ + 38×─ + ┐                                    (80 vw)
    ///   outer content: │ + <76 vw> + │                                  (80 vw)
    ///   inner frame  : [2 sp] + ┌ + 33×─ + ┐ + [4 sp]  →  76 vw content
    ///   inner content: [2 sp] + │ + <68 vw> + │ + [2 sp] →  76 vw content
    ///   connector    : [37 sp] + │/▼ + [1 sp] + label + trailing       (80 vw)
    ///
    /// For other total_vw values the constants are computed proportionally.
    class DiagramGenerator
    {
    public:
        // -----------------------------------------------------------------------
        // Construction
        // -----------------------------------------------------------------------

        /// @param total_vw  Target visual width for every output line (default 80).
        explicit DiagramGenerator(int total_vw = 80) : total_vw_(total_vw)
        {
            if (total_vw < 20)
                throw std::invalid_argument("total_vw must be at least 20");

            // Outer │ chars each = 2 vw → inner content = total_vw - 4
            inner_vw_ = total_vw_ - 4;

            // Outer hline count: ┌(2) + N×─(2) + ┐(2) = total_vw
            //   → N = (total_vw - 4) / 2
            outer_hline_count_ = (total_vw_ - 4) / 2;

            // Inner frame: "  ┌" + M×─ + "┐" + trailing_spaces = inner_vw_
            //   2(spaces) + 2(┌) + 2M(─) + 2(┐) + trailing = inner_vw_
            //   trailing = inner_vw_ - 6 - 2M
            // Choose M so trailing >= 2:  M = (inner_vw_ - 8) / 2
            inner_hline_count_ = (inner_vw_ - 8) / 2;
            inner_frame_trailing_ = inner_vw_ - 2 - 2 - inner_hline_count_ * 2 - 2;

            // Inner content text width:
            //   "  │" + text(T vw) + "│  " = inner_vw_
            //   2 + 2 + T + 2 + 2 = inner_vw_  → T = inner_vw_ - 8
            inner_text_vw_ = inner_vw_ - 8;

            // Connector: spaces before │/▼ symbol
            //   We align the connector at the visual center of the inner frame's
            //   horizontal line.  Inner ┌ starts at vw offset 4 (2 spaces + 2 for ┌),
            //   spans inner_hline_count_ × 2 columns.  Center = 4 + inner_hline_count_.
            connector_prefix_spaces_ = 2 + 2 + inner_hline_count_;
            // connector_prefix_spaces_ + 2(sym) + 1(space) + label + trailing = inner_vw_
        }

        // -----------------------------------------------------------------------
        // Building blocks
        // -----------------------------------------------------------------------

        /// Begin the outer enclosing box; optionally print a centred title row.
        void outer_box_begin(std::string_view title = "")
        {
            // Top border
            std::string top = "\xe2\x94\x8c"; // ┌
            top += make_hline(outer_hline_count_);
            top += "\xe2\x94\x90"; // ┐
            lines_.push_back(std::move(top));

            // Optional title
            if (!title.empty())
            {
                int tv = visual_width(title);
                int lpad = (inner_vw_ - tv) / 2;
                int rpad = inner_vw_ - tv - lpad;
                std::string content(static_cast<std::size_t>(lpad), ' ');
                content += title;
                content.append(static_cast<std::size_t>(rpad), ' ');
                lines_.push_back(make_outer_row(content));
            }

            // Blank separator
            lines_.push_back(make_blank_outer_row());
        }

        /// End the outer enclosing box.
        void outer_box_end()
        {
            lines_.push_back(make_blank_outer_row());

            std::string bot = "\xe2\x94\x94"; // └
            bot += make_hline(outer_hline_count_);
            bot += "\xe2\x94\x98"; // ┘
            lines_.push_back(std::move(bot));
        }

        /// Add an inner module box with 1–3 content lines.
        /// @param line1  First content line (layer header).
        /// @param line2  Second content line (optional).
        /// @param line3  Third content line (optional).
        void inner_box(std::string_view line1,
                       std::string_view line2 = "",
                       std::string_view line3 = "")
        {
            lines_.push_back(make_inner_top());
            lines_.push_back(make_inner_row("  " + std::string(line1)));
            if (!line2.empty())
                lines_.push_back(make_inner_row("    " + std::string(line2)));
            if (!line3.empty())
                lines_.push_back(make_inner_row("    " + std::string(line3)));
            lines_.push_back(make_inner_bottom());
        }

        /// Add a vertical connector line (│) with an optional label to the right.
        void connector_down(std::string_view label = "")
        {
            lines_.push_back(make_connector("\xe2\x94\x82", label)); // │
        }

        /// Add a downward arrow connector (▼) with an optional label to the right.
        void connector_arrow(std::string_view label = "")
        {
            lines_.push_back(make_connector("\xe2\x96\xbc", label)); // ▼
        }

        /// Add a blank outer row (empty line inside the outer box).
        void blank_line()
        {
            lines_.push_back(make_blank_outer_row());
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
                if (i > 0)
                    oss << '\n';
                oss << lines_[i];
            }
            return oss.str();
        }

        /// Return each line as a separate string.
        const std::vector<std::string> &lines() const noexcept { return lines_; }

        // -----------------------------------------------------------------------
        // Accessors (useful for unit tests)
        // -----------------------------------------------------------------------
        int total_vw() const noexcept { return total_vw_; }
        int inner_vw() const noexcept { return inner_vw_; }
        int inner_text_vw() const noexcept { return inner_text_vw_; }
        int connector_prefix() const noexcept { return connector_prefix_spaces_; }

    private:
        // -----------------------------------------------------------------------
        // Internal helpers
        // -----------------------------------------------------------------------

        /// Wrap content (must have visual width == inner_vw_) with outer │ chars.
        std::string make_outer_row(std::string_view content) const
        {
            std::string row;
            row.reserve(content.size() + 6);
            row += "\xe2\x94\x82"; // │
            row += content;
            row += "\xe2\x94\x82"; // │
            return row;
        }

        /// Build outer row from content string, padding to inner_vw_ first.
        std::string make_outer_row_padded(std::string_view content) const
        {
            return make_outer_row(pad_to_visual(content, inner_vw_));
        }

        std::string make_blank_outer_row() const
        {
            return make_outer_row(std::string(static_cast<std::size_t>(inner_vw_), ' '));
        }

        std::string make_inner_top() const
        {
            // "  ┌" + M×─ + "┐" + trailing_spaces
            std::string content = "  ";
            content += "\xe2\x94\x8c"; // ┌
            content += make_hline(inner_hline_count_);
            content += "\xe2\x94\x90"; // ┐
            content.append(static_cast<std::size_t>(inner_frame_trailing_), ' ');
            return make_outer_row(content);
        }

        std::string make_inner_bottom() const
        {
            // "  └" + M×─ + "┘" + trailing_spaces
            std::string content = "  ";
            content += "\xe2\x94\x94"; // └
            content += make_hline(inner_hline_count_);
            content += "\xe2\x94\x98"; // ┘
            content.append(static_cast<std::size_t>(inner_frame_trailing_), ' ');
            return make_outer_row(content);
        }

        std::string make_inner_row(std::string_view text) const
        {
            // "  │" + text(inner_text_vw_) + "│  "
            std::string inner_text = pad_to_visual(text, inner_text_vw_);
            std::string content = "  ";
            content += "\xe2\x94\x82"; // │
            content += inner_text;
            content += "\xe2\x94\x82"; // │
            content += "  ";
            return make_outer_row(content);
        }

        std::string make_connector(std::string_view sym_utf8,
                                   std::string_view label) const
        {
            // connector_prefix_spaces_ × ' ' + sym(2) + ' ' + label + trailing
            std::string content(static_cast<std::size_t>(connector_prefix_spaces_), ' ');
            content += sym_utf8; // │ or ▼  (2 vw)
            content += ' ';
            content += label;
            int cur = visual_width(content);
            int trailing = inner_vw_ - cur;
            if (trailing > 0)
                content.append(static_cast<std::size_t>(trailing), ' ');
            return make_outer_row(content);
        }

        // -----------------------------------------------------------------------
        // State
        // -----------------------------------------------------------------------
        int total_vw_;
        int inner_vw_;
        int outer_hline_count_;
        int inner_hline_count_;
        int inner_frame_trailing_;
        int inner_text_vw_;
        int connector_prefix_spaces_;

        std::vector<std::string> lines_;
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

    gen.outer_box_begin("pulseTrader Process");

    gen.inner_box("Layer 4: HeartbeatScheduler  (every 5 min)",
                  "└─► TaskQueue ──► AIAnalyzer ──► ParamAdvisor");
    gen.connector_down("param updates (atomic writes)");

    gen.inner_box("Layer 3: Strategy Engine",
                  "MomentumScalper | OrderBookScalper | MeanReversionScalper",
                  "SignalAggregator (weighted voting)");
    gen.connector_down("signals");

    gen.inner_box("Layer 6: Risk Management",
                  "RiskManager | PositionManager | StopLoss/TakeProfit Engines");
    gen.connector_down("approved orders");

    gen.inner_box("Layer 7: Order Execution",
                  "OrderExecutor | OrderTracker | ExecutionReport");
    gen.connector_down();

    gen.inner_box("Layer 8: Logging & Monitoring",
                  "Logger | TradeRecorder | MetricsCollector | AlertManager");
    gen.blank_line();

    gen.inner_box("Layer 2: Market Data  (hot path -- dedicated thread)",
                  "MarketFeed | OrderBookManager | KlineBuffer | TickerCache");
    gen.connector_down();

    gen.inner_box("Layer 1: Exchange  (Gate.io REST + WebSocket)",
                  "GateRestClient | GateWsClient | GateWsChannels | GateAuth");

    gen.outer_box_end();

    std::cout << gen.render() << '\n';

    // Verify all lines
    bool all_ok = true;
    for (const auto &line : gen.lines())
    {
        int vw = pulse::visual_width(line);
        if (vw != 80)
        {
            std::cerr << "FAIL vw=" << vw << " : " << line << '\n';
            all_ok = false;
        }
    }
    if (all_ok)
        std::cerr << "OK — all " << gen.lines().size()
                  << " lines have visual width 80\n";

    return all_ok ? 0 : 1;
}
#endif // DIAGRAM_GEN_MAIN
