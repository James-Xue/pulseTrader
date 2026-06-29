// main.ts — Application entry point
//
// Initializes WebSocket connection, DataStore, Golden Layout,
// and registers all 8 dashboard panels.

import './styles/main.css';
import './styles/golden-layout-overrides.css';
import './styles/panels.css';

import { WsClient } from './ws-client';
import { DataStore } from './data-store';
import { LayoutManager } from './layout-manager';
import { AccountBar } from './panels/account-bar';
import { OrderBookPanel } from './panels/order-book';
import { KlinePanel } from './panels/kline';
import { PositionsPanel } from './panels/positions';
import { OrdersPanel } from './panels/orders';
import { MetricsPanel } from './panels/metrics';
import { AiAnalysisPanel } from './panels/ai-analysis';
import { StrategiesPanel } from './panels/strategies';
import { RiskPanel } from './panels/risk';

class App {
    private wsClient: WsClient;
    private dataStore: DataStore;
    private layoutManager: LayoutManager;

    constructor() {
        // --- Auth token ---
        const params = new URLSearchParams(window.location.search);
        const urlToken = params.get('token');
        if (urlToken) localStorage.setItem('pulseToken', urlToken);
        const token = localStorage.getItem('pulseToken') || '';

        // --- WebSocket ---
        const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = proto + '//' + window.location.host + '/ws?token=' + encodeURIComponent(token);
        this.wsClient = new WsClient(wsUrl);

        // --- Data store ---
        this.dataStore = new DataStore();

        // Wire WS → DataStore
        this.wsClient.onSnapshot((snap) => this.dataStore.update(snap));
        this.wsClient.onStatus((connected) => this.dataStore.setConnectionStatus(connected));

        // --- Connection status indicator ---
        this.dataStore.onConnectionStatus((connected) => {
            const el = document.getElementById('connection-status');
            const text = document.getElementById('status-text');
            if (el && text) {
                el.className = connected ? 'status-connected' : 'status-disconnected';
                text.textContent = connected ? 'Connected' : 'Disconnected';
            }
        });

        // --- Halt banner ---
        this.dataStore.subscribe('risk', (risk) => {
            const banner = document.getElementById('halt-banner');
            const reason = document.getElementById('halt-reason');
            if (!banner || !reason) return;

            if (risk.trading_halted) {
                const reasons: Record<number, string> = {
                    1: 'Daily drawdown limit exceeded',
                    2: 'Peak-to-valley drawdown limit exceeded',
                    3: 'Manual halt',
                };
                reason.textContent = ' — ' + (reasons[risk.halt_reason] || `Unknown (code ${risk.halt_reason})`);
                banner.classList.remove('hidden');
            } else {
                banner.classList.add('hidden');
            }
        });

        // --- Account bar (fixed, not in GL) ---
        new AccountBar(this.dataStore);

        // --- Golden Layout ---
        const container = document.getElementById('layout-container')!;
        this.layoutManager = new LayoutManager(container);

        // Register all panel factories
        this.layoutManager.registerFactory('order-book', 'Order Book',
            () => new OrderBookPanel(this.dataStore));
        this.layoutManager.registerFactory('kline', 'K-Line Chart',
            () => new KlinePanel(this.dataStore));
        this.layoutManager.registerFactory('positions', 'Positions',
            () => new PositionsPanel(this.dataStore));
        this.layoutManager.registerFactory('orders', 'Orders',
            () => new OrdersPanel(this.dataStore));
        this.layoutManager.registerFactory('metrics', 'PnL & Metrics',
            () => new MetricsPanel(this.dataStore));
        this.layoutManager.registerFactory('ai-analysis', 'AI Analysis',
            () => new AiAnalysisPanel(this.dataStore));
        this.layoutManager.registerFactory('strategies', 'Strategies',
            () => new StrategiesPanel(this.dataStore));
        this.layoutManager.registerFactory('risk', 'Risk Status',
            () => new RiskPanel(this.dataStore));

        // Initialize layout (loads saved or default)
        this.layoutManager.init();

        // Save layout on close
        window.addEventListener('beforeunload', () => {
            this.layoutManager.save();
        });

        // Resize layout on window resize
        window.addEventListener('resize', () => {
            // Golden Layout handles this internally with autoSize
        });
    }
}

// Boot
document.addEventListener('DOMContentLoaded', () => {
    new App();
});
