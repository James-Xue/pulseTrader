// order-book.ts — Order book panel with bid/ask tables and volume bars

import { BasePanel } from './base-panel';
import type { DataStore } from '../data-store';
import type { OrderBook } from '../types';
import { fmt, escHtml } from '../utils';

const MAX_LEVELS = 10;

export class OrderBookPanel extends BasePanel<'order_book'> {
    readonly componentType = 'order-book';
    readonly title = 'Order Book';
    readonly field = 'order_book' as const;

    private headerEl: HTMLElement | null = null;
    private asksEl: HTMLElement | null = null;
    private spreadEl: HTMLElement | null = null;
    private bidsEl: HTMLElement | null = null;

    constructor(dataStore: DataStore) {
        super(dataStore);
    }

    protected buildDOM(): void {
        if (!this.container) return;

        this.container.innerHTML = `
            <div class="order-book-panel">
                <div class="ob-header" data-ref="header"></div>
                <div class="panel-scroll">
                    <div class="ob-section">
                        <div class="ob-section-title">Asks</div>
                        <table class="ob-table">
                            <thead>
                                <tr><th>Price</th><th>Qty</th><th>Cum Qty</th></tr>
                            </thead>
                            <tbody data-ref="asks"></tbody>
                        </table>
                    </div>
                    <div class="ob-spread" data-ref="spread">—</div>
                    <div class="ob-section">
                        <div class="ob-section-title">Bids</div>
                        <table class="ob-table">
                            <thead>
                                <tr><th>Price</th><th>Qty</th><th>Cum Qty</th></tr>
                            </thead>
                            <tbody data-ref="bids"></tbody>
                        </table>
                    </div>
                </div>
            </div>
        `;

        this.headerEl = this.container.querySelector('[data-ref="header"]');
        this.asksEl = this.container.querySelector('[data-ref="asks"]');
        this.spreadEl = this.container.querySelector('[data-ref="spread"]');
        this.bidsEl = this.container.querySelector('[data-ref="bids"]');
    }

    protected update(data: OrderBook): void {
        if (!data) return;

        if (this.headerEl) {
            this.headerEl.textContent = escHtml(data.symbol || '');
        }

        const asks = (data.asks || []).slice(0, MAX_LEVELS);
        const bids = (data.bids || []).slice(0, MAX_LEVELS);

        // Find max qty across all levels for bar width scaling
        let maxQty = 0;
        for (const a of asks) maxQty = Math.max(maxQty, a.quantity);
        for (const b of bids) maxQty = Math.max(maxQty, b.quantity);
        if (maxQty === 0) maxQty = 1;

        // Render asks — reversed so lowest ask is at the bottom
        if (this.asksEl) {
            const displayAsks = [...asks].reverse();
            let cumQty = 0;
            // Compute cumulative from the top (best ask side)
            const askCum: number[] = [];
            for (let i = asks.length - 1; i >= 0; i--) {
                cumQty += asks[i].quantity;
                askCum[i] = cumQty;
            }

            let html = '';
            for (let i = 0; i < displayAsks.length; i++) {
                const entry = displayAsks[i];
                const origIdx = asks.length - 1 - i;
                const pct = (entry.quantity / maxQty) * 100;
                html += `<tr>
                    <td class="ob-price sell">
                        <div class="ob-bar sell" style="width:${pct.toFixed(1)}%"></div>
                        <span>${fmt(entry.price, 2)}</span>
                    </td>
                    <td class="ob-qty">${fmt(entry.quantity, 4)}</td>
                    <td class="ob-cum">${fmt(askCum[origIdx], 4)}</td>
                </tr>`;
            }
            this.asksEl.innerHTML = html;
        }

        // Render bids
        if (this.bidsEl) {
            let cumQty = 0;
            let html = '';
            for (const entry of bids) {
                cumQty += entry.quantity;
                const pct = (entry.quantity / maxQty) * 100;
                html += `<tr>
                    <td class="ob-price buy">
                        <div class="ob-bar buy" style="width:${pct.toFixed(1)}%"></div>
                        <span>${fmt(entry.price, 2)}</span>
                    </td>
                    <td class="ob-qty">${fmt(entry.quantity, 4)}</td>
                    <td class="ob-cum">${fmt(cumQty, 4)}</td>
                </tr>`;
            }
            this.bidsEl.innerHTML = html;
        }

        // Spread
        if (this.spreadEl && asks.length > 0 && bids.length > 0) {
            const spread = asks[0].price - bids[0].price;
            const spreadBps = bids[0].price > 0
                ? ((spread / bids[0].price) * 10000).toFixed(1)
                : '—';
            this.spreadEl.textContent = `Spread: ${fmt(spread, 2)} (${spreadBps} bps)`;
        }
    }
}
