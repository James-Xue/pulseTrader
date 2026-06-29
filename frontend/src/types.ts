// types.ts — TypeScript interfaces for DashboardSnapshot
//
// Mirrors the C++ snapshot_types.hpp structure sent via WebSocket.

export interface OrderBookEntry {
    price: number;
    quantity: number;
}

export interface OrderBook {
    symbol: string;
    bids: OrderBookEntry[];
    asks: OrderBookEntry[];
    sequence_id: number;
    timestamp: number;
}

export interface Candle {
    open_time: number;
    close_time: number;
    open: number;
    high: number;
    low: number;
    close: number;
    volume: number;
    closed: boolean;
}

export interface KLine {
    symbol: string;
    candles: Candle[];
}

export interface Position {
    position_id: string;
    symbol: string;
    side: 'buy' | 'sell';
    quantity: number;
    entry_price: number;
    current_price: number;
    unrealized_pnl: number;
    notional_value: number;
    open_time: number;
    strategy_id: string;
}

export interface PortfolioSummary {
    open_position_count: number;
    total_notional: number;
    total_unrealized_pnl: number;
    net_exposure: number;
}

export interface Positions {
    positions: Position[];
    portfolio: PortfolioSummary | null;
}

export interface Order {
    order_id: string;
    symbol: string;
    side: 'buy' | 'sell';
    type: 'market' | 'limit' | 'post_only';
    requested_qty: number;
    filled_qty: number;
    status: 'pending' | 'open' | 'filled' | 'cancelled';
    submit_time: number;
    last_update_time: number;
}

export interface ExecutionReport {
    order_id: string;
    symbol: string;
    side: 'buy' | 'sell';
    filled_qty: number;
    avg_fill_price: number;
    slippage_bps: number;
    fees: number;
}

export interface Orders {
    active_orders: Order[];
    recent_reports: ExecutionReport[];
}

export interface Account {
    available: boolean;
    total: number;
    available_balance: number;
    unrealised_pnl: number;
    position_margin: number;
    order_margin: number;
    currency: string;
    spot_available: boolean;
    spot_total: number;
    spot_available_balance: number;
    spot_currency: string;
}

export interface Metrics {
    available: boolean;
    net_pnl: number;
    gross_pnl: number;
    win_rate: number;
    avg_win_loss_ratio: number;
    sharpe_ratio: number;
    max_drawdown: number;
    trade_count: number;
}

export interface ParamDelta {
    [key: string]: number;
}

export interface AnalysisResult {
    sentiment: 'bullish' | 'bearish' | 'neutral';
    direction_bias: number;
    volatility: 'high' | 'medium' | 'low';
    confidence: number;
    param_deltas: ParamDelta | null;
}

export interface AIAnalysis {
    available: boolean;
    result: AnalysisResult;
    last_update_ms: number;
}

export interface Strategy {
    name: string;
    id: string;
    symbol: string;
    enabled: boolean;
    running: boolean;
    poll_interval_ms: number;
}

export interface Strategies {
    strategies: Strategy[];
}

export interface Risk {
    trading_halted: boolean;
    halt_reason: number;
    daily_drawdown: number;
    max_drawdown: number;
    rate_limiter_tokens: number;
    rate_limiter_exhausted: boolean;
    portfolio: PortfolioSummary | null;
    open_position_count: number;
}

export interface DashboardSnapshot {
    timestamp_ms: number;
    account: Account;
    order_book: OrderBook;
    kline: KLine;
    positions: Positions;
    orders: Orders;
    metrics: Metrics;
    ai: AIAnalysis;
    strategies: Strategies;
    risk: Risk;
}
