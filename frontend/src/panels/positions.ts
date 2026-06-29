// positions.ts — Positions panel with portfolio summary

import { BasePanel } from './base-panel';
import type { DataStore } from '../data-store';
import type { Positions, Position, PortfolioSummary } from '../types';
import { fmt, pnlClass, escHtml } from '../utils';

export class PositionsPanel extends BasePanel<'positions'> {
    readonly componentType = 'positions';
    readonly title = 'Positions';
    readonly field = 'positions' as const;

    private tableEl: HTMLElement | null = null;
    private summaryEl: HTMLElement | null = null;

    constructor(dataStore: DataStore) {
        super(dataStore);
    }

    protected buildDOM(): void {
        if (!this.container) return;

        this.container.innerHTML = `
            <div class="positions-panel">
                <div class="panel-scroll">
                    <table class="panel-table">
                        <thead>
                            <tr>
                                <th>Symbol</th>
                                <th>Side</th>
                                <th>Qty</th>
                                <th>Entry</th>
                                <th>Current</th>
                                <th>uPnL</th>
                                <th>Notional</th>
                            </tr>
                        </thead>
                        <tbody data-ref="table"></tbody>
                    </table>
                    <div class="positions-empty" data-ref="empty" style="display:none">
                        No open positions
                    </div>
                </div>
                <div class="portfolio-summary" data-ref="summary"></div>
            </div>
        `;

        this.tableEl = this.container.querySelector('[data-ref="table"]');
        this.summaryEl = this.container.querySelector('[data-ref="summary"]');
    }

    protected update(data: Positions): void {
        if (!data) return;

        const positions = data.positions || [];

        // Render table
        if (this.tableEl) {
            if (positions.length === 0) {
                this.tableEl.innerHTML = '';
                const emptyEl = this.container?.querySelector('[data-ref="empty"]') as HTMLElement;
                if (emptyEl) emptyEl.style.display = 'block';
            } else {
                const emptyEl = this.container?.querySelector('[data-ref="empty"]') as HTMLElement;
                if (emptyEl) emptyEl.style.display = 'none';

                this.tableEl.innerHTML = positions.map((p) => this.renderRow(p)).join('');
            }
        }

        // Render portfolio summary
        if (this.summaryEl && data.portfolio) {
            this.summaryEl.innerHTML = this.renderSummary(data.portfolio);
        }
    }

    private renderRow(p: Position): string {
        const sideClass = p.side === 'buy' ? 'positive' : 'negative';
        const pnlCls = pnlClass(p.unrealized_pnl);
        return `<tr>
            <td>${escHtml(p.symbol)}</td>
            <td class="${sideClass}">${escHtml(p.side.toUpperCase())}</td>
            <td>${fmt(p.quantity, 4)}</td>
            <td>${fmt(p.entry_price, 2)}</td>
            <td>${fmt(p.current_price, 2)}</td>
            <td class="${pnlCls}">${fmt(p.unrealized_pnl, 2)}</td>
            <td>${fmt(p.notional_value, 2)}</td>
        </tr>`;
    }

    private renderSummary(s: PortfolioSummary): string {
        const pnlCls = pnlClass(s.total_unrealized_pnl);
        return `
            <div class="summary-grid">
                <div class="summary-item">
                    <span class="summary-label">Positions</span>
                    <span class="summary-value">${s.open_position_count}</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">Total Notional</span>
                    <span class="summary-value">${fmt(s.total_notional, 2)}</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">Unrealized PnL</span>
                    <span class="summary-value ${pnlCls}">${fmt(s.total_unrealized_pnl, 2)}</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">Net Exposure</span>
                    <span class="summary-value">${fmt(s.net_exposure, 2)}</span>
                </div>
            </div>
        `;
    }
}
