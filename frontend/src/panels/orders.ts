// orders.ts — Active orders and recent fills panel

import { BasePanel } from './base-panel';
import type { DataStore } from '../data-store';
import type { Orders, Order, ExecutionReport } from '../types';
import { fmt, escHtml } from '../utils';

const MAX_RECENT_FILLS = 10;

export class OrdersPanel extends BasePanel<'orders'> {
    readonly componentType = 'orders';
    readonly title = 'Orders';
    readonly field = 'orders' as const;

    private activeEl: HTMLElement | null = null;
    private fillsEl: HTMLElement | null = null;

    constructor(dataStore: DataStore) {
        super(dataStore);
    }

    protected buildDOM(): void {
        if (!this.container) return;

        this.container.innerHTML = `
            <div class="orders-panel">
                <div class="panel-scroll">
                    <div class="orders-section">
                        <div class="orders-section-title">Active Orders</div>
                        <table class="panel-table">
                            <thead>
                                <tr>
                                    <th>Order ID</th>
                                    <th>Symbol</th>
                                    <th>Side</th>
                                    <th>Type</th>
                                    <th>Req Qty</th>
                                    <th>Fill Qty</th>
                                    <th>Status</th>
                                </tr>
                            </thead>
                            <tbody data-ref="active"></tbody>
                        </table>
                        <div class="orders-empty" data-ref="active-empty" style="display:none">
                            No active orders
                        </div>
                    </div>
                    <div class="orders-section">
                        <div class="orders-section-title">Recent Fills</div>
                        <table class="panel-table">
                            <thead>
                                <tr>
                                    <th>Order ID</th>
                                    <th>Symbol</th>
                                    <th>Side</th>
                                    <th>Filled Qty</th>
                                    <th>Avg Price</th>
                                    <th>Slippage (bps)</th>
                                    <th>Fees</th>
                                </tr>
                            </thead>
                            <tbody data-ref="fills"></tbody>
                        </table>
                        <div class="orders-empty" data-ref="fills-empty" style="display:none">
                            No recent fills
                        </div>
                    </div>
                </div>
            </div>
        `;

        this.activeEl = this.container.querySelector('[data-ref="active"]');
        this.fillsEl = this.container.querySelector('[data-ref="fills"]');
    }

    protected update(data: Orders): void {
        if (!data) return;

        const active = data.activeOrders || [];
        const fills = (data.recentReports || []).slice(0, MAX_RECENT_FILLS);

        // Active orders
        if (this.activeEl) {
            if (active.length === 0) {
                this.activeEl.innerHTML = '';
                this.toggleEmpty('active-empty', true);
            } else {
                this.toggleEmpty('active-empty', false);
                this.activeEl.innerHTML = active.map((o) => this.renderOrderRow(o)).join('');
            }
        }

        // Recent fills
        if (this.fillsEl) {
            if (fills.length === 0) {
                this.fillsEl.innerHTML = '';
                this.toggleEmpty('fills-empty', true);
            } else {
                this.toggleEmpty('fills-empty', false);
                this.fillsEl.innerHTML = fills.map((r) => this.renderFillRow(r)).join('');
            }
        }
    }

    private renderOrderRow(o: Order): string {
        const sideClass = o.side === 'buy' ? 'positive' : 'negative';
        const shortId = escHtml(o.order_id.slice(0, 8));
        return `<tr>
            <td class="mono">${shortId}</td>
            <td>${escHtml(o.symbol)}</td>
            <td class="${sideClass}">${escHtml(o.side.toUpperCase())}</td>
            <td>${escHtml(o.type.toUpperCase())}</td>
            <td>${fmt(o.requested_qty, 4)}</td>
            <td>${fmt(o.filled_qty, 4)}</td>
            <td><span class="badge badge-blue">${escHtml(o.status.toUpperCase())}</span></td>
        </tr>`;
    }

    private renderFillRow(r: ExecutionReport): string {
        const sideClass = r.side === 'buy' ? 'positive' : 'negative';
        const shortId = escHtml(r.order_id.slice(0, 8));
        return `<tr>
            <td class="mono">${shortId}</td>
            <td>${escHtml(r.symbol)}</td>
            <td class="${sideClass}">${escHtml(r.side.toUpperCase())}</td>
            <td>${fmt(r.filled_qty, 4)}</td>
            <td>${fmt(r.avg_fill_price, 2)}</td>
            <td>${fmt(r.slippage_bps, 1)}</td>
            <td>${fmt(r.fees, 4)}</td>
        </tr>`;
    }

    private toggleEmpty(ref: string, show: boolean): void {
        const el = this.container?.querySelector(`[data-ref="${ref}"]`) as HTMLElement;
        if (el) el.style.display = show ? 'block' : 'none';
    }
}
