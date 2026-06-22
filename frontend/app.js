// app.js — pulseTrader WebUI dashboard (Layer 9)
//
// Vanilla JavaScript SPA that connects to the WebSocket server and renders
// real-time DashboardSnapshot updates across 8 panels.
//
// No framework dependencies. Targets modern browsers (ES2020+).

(function () {
    'use strict';

    // =========================================================================
    // Connection Manager
    // =========================================================================

    let ws = null;
    let reconnectDelay = 1000; // Start at 1s, double each attempt, cap at 30s
    const RECONNECT_CAP = 30000;
    let reconnectTimer = null;
    let authToken = '';

    /**
     * Extract the auth token from URL query parameters, localStorage, or prompt the user.
     * Cached in localStorage so the prompt only appears once.
     */
    function getToken() {
        const params = new URLSearchParams(window.location.search);
        const urlToken = params.get('token');
        if (urlToken) {
            localStorage.setItem('pulseToken', urlToken);
            return urlToken;
        }
        return localStorage.getItem('pulseToken') || '';
    }

    /**
     * Update the connection status indicator in the header.
     */
    function setStatus(connected) {
        const el = document.getElementById('connection-status');
        const text = document.getElementById('status-text');
        if (connected) {
            el.className = 'status-connected';
            text.textContent = 'Connected';
        } else {
            el.className = 'status-disconnected';
            text.textContent = 'Disconnected';
        }
    }

    /**
     * Connect to the WebSocket server and wire up handlers.
     */
    function connect() {
        if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
            return;
        }

        const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const url = proto + '//' + window.location.host + '/ws?token=' + encodeURIComponent(authToken);

        ws = new WebSocket(url);

        ws.onopen = function () {
            setStatus(true);
            reconnectDelay = 1000; // Reset backoff on successful connection
        };

        ws.onmessage = function (event) {
            try {
                const snap = JSON.parse(event.data);
                dispatch(snap);
            } catch (e) {
                console.error('[pulseTrader] Failed to parse snapshot:', e);
            }
        };

        ws.onclose = function () {
            setStatus(false);
            scheduleReconnect();
        };

        ws.onerror = function () {
            setStatus(false);
            // onclose will fire after onerror, so reconnect is handled there
        };
    }

    /**
     * Schedule a reconnection attempt with exponential backoff.
     */
    function scheduleReconnect() {
        if (reconnectTimer) {
            clearTimeout(reconnectTimer);
        }
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, reconnectDelay);
        reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_CAP);
    }

    // =========================================================================
    // Message Dispatcher
    // =========================================================================

    function dispatch(snap) {
        renderAccount(snap.account);
        renderOrderBook(snap.order_book);
        renderKline(snap.kline);
        renderPositions(snap.positions);
        renderOrders(snap.orders);
        renderMetrics(snap.metrics);
        renderAi(snap.ai);
        renderStrategies(snap.strategies);
        renderRisk(snap.risk);
        updateHaltBanner(snap.risk);
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    function fmt(n, decimals) {
        if (decimals === undefined) { decimals = 2; }
        if (n === null || n === undefined || isNaN(n)) { return '—'; }
        return Number(n).toFixed(decimals);
    }

    function fmtPct(n) {
        return (n * 100).toFixed(2) + '%';
    }

    function pnlClass(v) {
        if (v > 0) { return 'positive'; }
        if (v < 0) { return 'negative'; }
        return '';
    }

    function escHtml(s) {
        var div = document.createElement('div');
        div.appendChild(document.createTextNode(s));
        return div.innerHTML;
    }

    function setContent(id, html) {
        var el = document.getElementById(id);
        if (el) { el.innerHTML = html; }
    }

    function msToTime(ms) {
        if (!ms) { return '—'; }
        var d = new Date(ms);
        return d.toLocaleTimeString();
    }

    // =========================================================================
    // Account Balance Bar
    // =========================================================================

    function renderAccount(acct) {
        var bar = document.getElementById('account-bar');
        if (!bar) { return; }

        var hasFutures = acct && acct.available;
        var hasSpot = acct && acct.spot_available;

        if (!hasFutures && !hasSpot) {
            bar.classList.add('hidden');
            return;
        }

        bar.classList.remove('hidden');

        // --- Futures ---
        if (hasFutures) {
            var cur = acct.currency || 'USDT';
            document.getElementById('account-total').textContent =
                fmt(acct.total) + ' ' + cur;
            document.getElementById('account-available').textContent =
                fmt(acct.available_balance) + ' ' + cur;

            var pnlEl = document.getElementById('account-pnl');
            pnlEl.textContent = (acct.unrealised_pnl >= 0 ? '+' : '') +
                fmt(acct.unrealised_pnl) + ' ' + cur;
            pnlEl.className = 'account-value ' + pnlClass(acct.unrealised_pnl);

            var marginUsed = (acct.position_margin || 0) + (acct.order_margin || 0);
            document.getElementById('account-margin').textContent =
                fmt(marginUsed) + ' ' + cur;
        }

        // --- Spot ---
        if (hasSpot) {
            var scur = acct.spot_currency || 'USDT';
            document.getElementById('account-spot-total').textContent =
                fmt(acct.spot_total) + ' ' + scur;
            document.getElementById('account-spot-available').textContent =
                fmt(acct.spot_available_balance) + ' ' + scur;
        }
    }

    // =========================================================================
    // Panel 1: Order Book
    // =========================================================================

    function renderOrderBook(ob) {
        if (!ob || !ob.symbol) {
            setContent('orderbook-content', '<p class="placeholder">Waiting for data...</p>');
            return;
        }

        var bids = ob.bids || [];
        var asks = ob.asks || [];

        // Compute cumulative volumes for bar width
        var maxQty = 0;
        var bidCum = 0;
        var askCum = 0;
        var bidCumArr = [];
        var askCumArr = [];

        for (var i = 0; i < bids.length; i++) {
            bidCum += bids[i].quantity;
            bidCumArr.push(bidCum);
            if (bids[i].quantity > maxQty) { maxQty = bids[i].quantity; }
        }
        for (var i = 0; i < asks.length; i++) {
            askCum += asks[i].quantity;
            askCumArr.push(askCum);
            if (asks[i].quantity > maxQty) { maxQty = asks[i].quantity; }
        }

        var html = '<div style="font-size:0.8em;color:#a0a0c0;margin-bottom:6px">' + escHtml(ob.symbol) + '</div>';

        // Asks (reversed so lowest at bottom)
        html += '<table><thead><tr><th>Price</th><th>Qty</th><th>Cum</th></tr></thead><tbody>';
        var askStart = Math.min(asks.length, 10);
        for (var i = askStart - 1; i >= 0; i--) {
            var barW = maxQty > 0 ? (asks[i].quantity / maxQty * 100) : 0;
            html += '<tr class="ob-ask">'
                + '<td class="mono sell-side">' + fmt(asks[i].price) + '</td>'
                + '<td class="mono">' + fmt(asks[i].quantity, 4) + '<div class="ob-bar ob-bar-ask" style="width:' + barW.toFixed(1) + '%"></div></td>'
                + '<td class="mono">' + fmt(askCumArr[i], 4) + '</td>'
                + '</tr>';
        }
        html += '</tbody></table>';

        // Spread
        var bestBid = bids.length > 0 ? bids[0].price : 0;
        var bestAsk = asks.length > 0 ? asks[0].price : 0;
        var spread = bestAsk > 0 && bestBid > 0 ? (bestAsk - bestBid) : 0;
        html += '<div class="ob-spread">Spread: <span class="mono">' + fmt(spread) + '</span></div>';

        // Bids
        html += '<table><tbody>';
        var bidShow = Math.min(bids.length, 10);
        for (var i = 0; i < bidShow; i++) {
            var barW = maxQty > 0 ? (bids[i].quantity / maxQty * 100) : 0;
            html += '<tr class="ob-bid">'
                + '<td class="mono buy-side">' + fmt(bids[i].price) + '</td>'
                + '<td class="mono">' + fmt(bids[i].quantity, 4) + '<div class="ob-bar ob-bar-bid" style="width:' + barW.toFixed(1) + '%"></div></td>'
                + '<td class="mono">' + fmt(bidCumArr[i], 4) + '</td>'
                + '</tr>';
        }
        html += '</tbody></table>';

        setContent('orderbook-content', html);
    }

    // =========================================================================
    // Panel 2: K-line Chart
    // =========================================================================

    function renderKline(kl) {
        if (!kl || !kl.symbol || !kl.candles || kl.candles.length === 0) {
            setContent('kline-content', '<p class="placeholder">Waiting for candle data...</p>');
            return;
        }

        var candles = kl.candles;
        var show = candles.slice(-50).reverse(); // Last 50, newest first

        var html = '<div style="font-size:0.8em;color:#a0a0c0;margin-bottom:6px">' + escHtml(kl.symbol) + '</div>';
        html += '<table><thead><tr><th>Time</th><th>O</th><th>H</th><th>L</th><th>C</th><th>V</th><th></th></tr></thead><tbody>';

        for (var i = 0; i < show.length; i++) {
            var c = show[i];
            var up = c.close >= c.open;
            var cls = up ? 'kline-up' : 'kline-down';
            var arrow = up ? '▲' : '▼';

            html += '<tr>'
                + '<td class="mono">' + msToTime(c.open_time) + '</td>'
                + '<td class="mono">' + fmt(c.open) + '</td>'
                + '<td class="mono">' + fmt(c.high) + '</td>'
                + '<td class="mono">' + fmt(c.low) + '</td>'
                + '<td class="mono ' + cls + '">' + fmt(c.close) + '</td>'
                + '<td class="mono">' + fmt(c.volume, 1) + '</td>'
                + '<td class="' + cls + '">' + (c.closed ? arrow : '…') + '</td>'
                + '</tr>';
        }

        html += '</tbody></table>';
        setContent('kline-content', html);
    }

    // =========================================================================
    // Panel 3: Open Positions
    // =========================================================================

    function renderPositions(pos) {
        if (!pos || !pos.positions || pos.positions.length === 0) {
            setContent('positions-content', '<p class="placeholder">No open positions</p>');
            return;
        }

        var positions = pos.positions;
        var html = '<table><thead><tr><th>Symbol</th><th>Side</th><th>Qty</th><th>Entry</th><th>Current</th><th>uPnL</th><th>Notional</th></tr></thead><tbody>';

        for (var i = 0; i < positions.length; i++) {
            var p = positions[i];
            var sideClass = p.side === 'buy' ? 'buy-side' : 'sell-side';
            var pnlCls = pnlClass(p.unrealized_pnl);

            html += '<tr>'
                + '<td>' + escHtml(p.symbol) + '</td>'
                + '<td class="' + sideClass + '">' + escHtml(p.side) + '</td>'
                + '<td class="mono">' + fmt(p.quantity, 4) + '</td>'
                + '<td class="mono">' + fmt(p.entry_price) + '</td>'
                + '<td class="mono">' + fmt(p.current_price) + '</td>'
                + '<td class="mono ' + pnlCls + '">' + fmt(p.unrealized_pnl, 4) + '</td>'
                + '<td class="mono">' + fmt(p.notional_value) + '</td>'
                + '</tr>';
        }
        html += '</tbody></table>';

        // Portfolio summary
        var pf = pos.portfolio;
        if (pf) {
            html += '<div class="portfolio-summary">'
                + '<div class="summary-row"><span class="summary-label">Positions</span><span class="mono">' + pf.open_position_count + '</span></div>'
                + '<div class="summary-row"><span class="summary-label">Total Notional</span><span class="mono">' + fmt(pf.total_notional) + '</span></div>'
                + '<div class="summary-row"><span class="summary-label">Unrealized PnL</span><span class="mono ' + pnlClass(pf.total_unrealized_pnl) + '">' + fmt(pf.total_unrealized_pnl, 4) + '</span></div>'
                + '<div class="summary-row"><span class="summary-label">Net Exposure</span><span class="mono">' + fmt(pf.net_exposure) + '</span></div>'
                + '</div>';
        }

        setContent('positions-content', html);
    }

    // =========================================================================
    // Panel 4: Active Orders
    // =========================================================================

    function renderOrders(orders) {
        if (!orders) {
            setContent('orders-content', '<p class="placeholder">No order data</p>');
            return;
        }

        var active = orders.active_orders || [];
        var reports = orders.recent_reports || [];
        var html = '';

        // Active orders
        if (active.length > 0) {
            html += '<div style="font-size:0.8em;color:#a0a0c0;margin-bottom:4px">Active</div>';
            html += '<table><thead><tr><th>ID</th><th>Symbol</th><th>Side</th><th>Type</th><th>Req</th><th>Fill</th><th>Status</th></tr></thead><tbody>';
            for (var i = 0; i < active.length; i++) {
                var o = active[i];
                var sideClass = o.side === 'buy' ? 'buy-side' : 'sell-side';
                html += '<tr>'
                    + '<td class="mono" style="font-size:0.75em">' + escHtml(o.order_id.substring(0, 8)) + '</td>'
                    + '<td>' + escHtml(o.symbol) + '</td>'
                    + '<td class="' + sideClass + '">' + escHtml(o.side) + '</td>'
                    + '<td>' + escHtml(o.type) + '</td>'
                    + '<td class="mono">' + fmt(o.requested_qty, 4) + '</td>'
                    + '<td class="mono">' + fmt(o.filled_qty, 4) + '</td>'
                    + '<td><span class="badge badge-blue">' + escHtml(o.status) + '</span></td>'
                    + '</tr>';
            }
            html += '</tbody></table>';
        } else {
            html += '<p class="placeholder">No active orders</p>';
        }

        // Recent fills
        if (reports.length > 0) {
            html += '<div style="font-size:0.8em;color:#a0a0c0;margin:10px 0 4px">Recent Fills</div>';
            html += '<table><thead><tr><th>ID</th><th>Symbol</th><th>Side</th><th>Filled</th><th>Avg Px</th><th>Slip</th><th>Fees</th></tr></thead><tbody>';
            for (var i = 0; i < Math.min(reports.length, 10); i++) {
                var r = reports[i];
                var sideClass = r.side === 'buy' ? 'buy-side' : 'sell-side';
                html += '<tr>'
                    + '<td class="mono" style="font-size:0.75em">' + escHtml(r.order_id.substring(0, 8)) + '</td>'
                    + '<td>' + escHtml(r.symbol) + '</td>'
                    + '<td class="' + sideClass + '">' + escHtml(r.side) + '</td>'
                    + '<td class="mono">' + fmt(r.filled_qty, 4) + '</td>'
                    + '<td class="mono">' + fmt(r.avg_fill_price) + '</td>'
                    + '<td class="mono">' + fmt(r.slippage_bps, 1) + ' bps</td>'
                    + '<td class="mono">' + fmt(r.fees, 4) + '</td>'
                    + '</tr>';
            }
            html += '</tbody></table>';
        }

        setContent('orders-content', html);
    }

    // =========================================================================
    // Panel 5: PnL & Metrics
    // =========================================================================

    function renderMetrics(m) {
        if (!m || !m.available) {
            setContent('metrics-content', '<p class="placeholder">Metrics not yet available</p>');
            return;
        }

        var html = '<div class="metrics-grid">';

        html += metricCard('Net PnL', fmt(m.net_pnl, 4), pnlClass(m.net_pnl));
        html += metricCard('Gross PnL', fmt(m.gross_pnl, 4), pnlClass(m.gross_pnl));
        html += metricCard('Win Rate', fmtPct(m.win_rate), '');
        html += metricCard('Win/Loss', fmt(m.avg_win_loss_ratio), '');
        html += metricCard('Sharpe', fmt(m.sharpe_ratio), m.sharpe_ratio >= 1 ? 'positive' : m.sharpe_ratio < 0 ? 'negative' : '');
        html += metricCard('Max DD', fmtPct(m.max_drawdown), m.max_drawdown > 0.03 ? 'negative' : '');
        html += metricCard('Trades', String(m.trade_count), '');

        html += '</div>';
        setContent('metrics-content', html);
    }

    function metricCard(label, value, cls) {
        return '<div class="metric-card">'
            + '<div class="metric-label">' + escHtml(label) + '</div>'
            + '<div class="metric-value ' + cls + '">' + value + '</div>'
            + '</div>';
    }

    // =========================================================================
    // Panel 6: AI Analysis
    // =========================================================================

    function renderAi(ai) {
        if (!ai || !ai.available) {
            setContent('ai-content', '<p class="placeholder">Awaiting first AI cycle...</p>');
            return;
        }

        var r = ai.result;
        var sentimentEmoji = '⚪';
        var sentimentText = 'Neutral';
        if (r.sentiment === 'bullish') { sentimentEmoji = '🟢'; sentimentText = 'Bullish'; }
        else if (r.sentiment === 'bearish') { sentimentEmoji = '🔴'; sentimentText = 'Bearish'; }

        var html = '<div class="ai-sentiment">' + sentimentEmoji + ' ' + sentimentText + '</div>';

        // Direction bias bar
        var biasPct = ((r.direction_bias + 1) / 2) * 100; // Map [-1,1] to [0,100]
        html += '<div class="ai-metric"><span class="ai-metric-label">Direction Bias</span><span class="ai-metric-value mono">' + fmt(r.direction_bias) + '</span></div>';
        html += '<div class="direction-bar-container"><div class="direction-bar-marker" style="left:' + biasPct.toFixed(1) + '%"></div></div>';

        // Volatility badge
        var volBadge = 'badge-gray';
        if (r.volatility === 'high') { volBadge = 'badge-red'; }
        else if (r.volatility === 'low') { volBadge = 'badge-green'; }
        html += '<div class="ai-metric"><span class="ai-metric-label">Volatility</span><span class="badge ' + volBadge + '">' + escHtml(r.volatility) + '</span></div>';

        // Confidence meter
        var confPct = (r.confidence * 100).toFixed(0);
        var confColor = r.confidence >= 0.7 ? 'green' : r.confidence >= 0.4 ? 'yellow' : 'red';
        html += '<div class="ai-metric"><span class="ai-metric-label">Confidence</span><span class="ai-metric-value mono">' + confPct + '%</span></div>';
        html += '<div class="gauge-container"><div class="gauge-fill gauge-fill-' + confColor + '" style="width:' + confPct + '%"></div></div>';

        // Last update
        html += '<div class="ai-metric" style="margin-top:8px"><span class="ai-metric-label">Last Update</span><span class="mono" style="font-size:0.85em">' + msToTime(ai.last_update_ms) + '</span></div>';

        // Param deltas
        if (r.param_deltas) {
            html += '<div style="font-size:0.8em;color:#a0a0c0;margin:10px 0 4px">Parameter Deltas</div>';
            html += '<table><thead><tr><th>Param</th><th>Delta</th></tr></thead><tbody>';
            var deltas = r.param_deltas;
            var keys = Object.keys(deltas);
            for (var i = 0; i < keys.length; i++) {
                var key = keys[i];
                var val = deltas[key];
                var shortKey = key.replace(/_delta$/, '').replace(/_/g, ' ');
                html += '<tr><td>' + escHtml(shortKey) + '</td><td class="mono ' + pnlClass(val) + '">' + (val >= 0 ? '+' : '') + fmt(val, 4) + '</td></tr>';
            }
            html += '</tbody></table>';
        }

        setContent('ai-content', html);
    }

    // =========================================================================
    // Panel 7: Strategies
    // =========================================================================

    function renderStrategies(strats) {
        if (!strats || !strats.strategies || strats.strategies.length === 0) {
            setContent('strategies-content', '<p class="placeholder">No strategies registered</p>');
            return;
        }

        var html = '';
        var list = strats.strategies;

        for (var i = 0; i < list.length; i++) {
            var s = list[i];
            var enabledBadge = s.enabled
                ? '<span class="badge badge-green">Enabled</span>'
                : '<span class="badge badge-gray">Disabled</span>';
            var runningBadge = s.running
                ? '<span class="badge badge-green badge-running">Running</span>'
                : '<span class="badge badge-gray">Stopped</span>';

            html += '<div class="strategy-card">'
                + '<div class="strategy-header">'
                + '<span class="strategy-name">' + escHtml(s.name) + '</span>'
                + '<span>' + runningBadge + '</span>'
                + '</div>'
                + '<div class="strategy-meta">'
                + '<span>' + escHtml(s.symbol) + '</span>'
                + '<span>' + s.poll_interval_ms + 'ms</span>'
                + enabledBadge
                + '</div>'
                + '<div style="font-size:0.75em;color:#666;margin-top:4px">' + escHtml(s.id) + '</div>'
                + '</div>';
        }

        setContent('strategies-content', html);
    }

    // =========================================================================
    // Panel 8: Risk Status
    // =========================================================================

    function renderRisk(risk) {
        if (!risk) {
            setContent('risk-content', '<p class="placeholder">Waiting for risk data...</p>');
            return;
        }

        var html = '';

        // Trading status
        var statusBadge = risk.trading_halted
            ? '<span class="badge badge-red">HALTED</span>'
            : '<span class="badge badge-green">ACTIVE</span>';
        html += '<div class="risk-section"><div class="risk-label">Trading Status</div>' + statusBadge + '</div>';

        // Daily drawdown gauge
        var ddPct = Math.min(risk.daily_drawdown * 100, 100);
        var ddColor = ddPct > 80 ? 'red' : ddPct > 50 ? 'yellow' : 'green';
        html += '<div class="risk-section">'
            + '<div class="risk-label">Daily Drawdown <span class="mono risk-value">' + fmtPct(risk.daily_drawdown) + '</span></div>'
            + '<div class="gauge-container"><div class="gauge-fill gauge-fill-' + ddColor + '" style="width:' + ddPct.toFixed(1) + '%"></div></div>'
            + '</div>';

        // Max drawdown gauge
        var mddPct = Math.min(risk.max_drawdown * 100, 100);
        var mddColor = mddPct > 80 ? 'red' : mddPct > 50 ? 'yellow' : 'green';
        html += '<div class="risk-section">'
            + '<div class="risk-label">Max Drawdown <span class="mono risk-value">' + fmtPct(risk.max_drawdown) + '</span></div>'
            + '<div class="gauge-container"><div class="gauge-fill gauge-fill-' + mddColor + '" style="width:' + mddPct.toFixed(1) + '%"></div></div>'
            + '</div>';

        // Rate limiter
        var rlBadge = risk.rate_limiter_exhausted
            ? '<span class="badge badge-red">Exhausted</span>'
            : '<span class="badge badge-green">' + fmt(risk.rate_limiter_tokens, 1) + ' tokens</span>';
        html += '<div class="risk-section"><div class="risk-label">Rate Limiter</div>' + rlBadge + '</div>';

        // Portfolio summary
        if (risk.portfolio) {
            var pf = risk.portfolio;
            html += '<div class="risk-section"><div class="risk-label">Portfolio</div>'
                + '<div style="font-size:0.85em">'
                + '<div class="summary-row"><span class="summary-label">Positions</span><span class="mono">' + risk.open_position_count + '</span></div>'
                + '<div class="summary-row"><span class="summary-label">Total Notional</span><span class="mono">' + fmt(pf.total_notional) + '</span></div>'
                + '<div class="summary-row"><span class="summary-label">Unrealized PnL</span><span class="mono ' + pnlClass(pf.total_unrealized_pnl) + '">' + fmt(pf.total_unrealized_pnl, 4) + '</span></div>'
                + '</div></div>';
        }

        setContent('risk-content', html);
    }

    // =========================================================================
    // Halt Banner
    // =========================================================================

    var haltReasons = {
        0: '',
        1: 'Daily drawdown limit exceeded',
        2: 'Peak-to-valley drawdown limit exceeded',
        3: 'Manual halt'
    };

    function updateHaltBanner(risk) {
        var banner = document.getElementById('halt-banner');
        var reason = document.getElementById('halt-reason');
        if (!banner || !reason) { return; }

        if (risk && risk.trading_halted) {
            var msg = haltReasons[risk.halt_reason] || 'Unknown reason (code ' + risk.halt_reason + ')';
            reason.textContent = ' — ' + msg;
            banner.classList.remove('hidden');
        } else {
            banner.classList.add('hidden');
        }
    }

    // =========================================================================
    // Bootstrap
    // =========================================================================

    authToken = getToken();
    connect();

})();
