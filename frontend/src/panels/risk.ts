import { BasePanel } from '../panels/base-panel';
import type { DataStore } from '../data-store';
import { fmt, fmtPct, pnlClass } from '../utils';
import type { Risk, PortfolioSummary } from '../types';

const HALT_REASONS: { [key: number]: string } = {
  0: '',
  1: 'Daily drawdown limit exceeded',
  2: 'Peak-to-valley drawdown limit exceeded',
  3: 'Manual halt',
};

export class RiskPanel extends BasePanel<'risk'> {
  readonly componentType = 'risk';
  readonly title = 'Risk Status';
  readonly field = 'risk' as const;

  constructor(dataStore: DataStore) {
    super(dataStore);
  }

  protected buildDOM(): void {
    if (!this.container) return;
    this.container.innerHTML = '';
  }

  protected update(data: Risk): void {
    if (!this.container) return;

    this.container.innerHTML = '';

    // Trading status
    const statusEl = document.createElement('div');
    statusEl.className = 'risk-status';
    if (data.trading_halted) {
      statusEl.innerHTML = `<span class="risk-status-label">Trading:</span><span class="badge badge-red">HALTED</span>`;
      const reason = HALT_REASONS[data.haltReason] || '';
      if (reason) {
        const reasonEl = document.createElement('div');
        reasonEl.className = 'risk-halt-reason';
        reasonEl.textContent = reason;
        statusEl.appendChild(reasonEl);
      }
    } else {
      statusEl.innerHTML = `<span class="risk-status-label">Trading:</span><span class="badge badge-green">ACTIVE</span>`;
    }
    this.container.appendChild(statusEl);

    // Daily drawdown gauge
    const dailyDdPct = data.maxDrawdown > 0
      ? (data.dailyDrawdown / data.maxDrawdown) * 100
      : 0;
    this.container.appendChild(this.renderGauge('Daily Drawdown', dailyDdPct, fmtPct(data.dailyDrawdown)));

    // Max drawdown gauge
    const maxDdPct = data.maxDrawdown * 100;
    this.container.appendChild(this.renderGauge('Max Drawdown', maxDdPct, fmtPct(data.maxDrawdown)));

    // Rate limiter
    const rateEl = document.createElement('div');
    rateEl.className = 'risk-rate-limiter';
    if (data.rate_limiter_exhausted) {
      rateEl.innerHTML = `<span class="risk-rate-limiter-label">Rate Limiter:</span><span class="badge badge-red">Exhausted</span>`;
    } else {
      rateEl.innerHTML = `<span class="risk-rate-limiter-label">Rate Limiter:</span><span class="badge badge-green">${data.rate_limiter_tokens} tokens</span>`;
    }
    this.container.appendChild(rateEl);

    // Portfolio summary
    if (data.portfolio) {
      this.container.appendChild(this.renderPortfolio(data.portfolio));
    }
  }

  private renderGauge(label: string, pct: number, displayValue: string): HTMLElement {
    const gaugeClass = pct > 80 ? 'gauge-fill-red' : pct >= 50 ? 'gauge-fill-yellow' : 'gauge-fill-green';
    const container = document.createElement('div');
    container.className = 'gauge-container';
    container.innerHTML = `
      <div class="gauge-label">${label}: ${displayValue}</div>
      <div class="gauge-track">
        <div class="gauge-fill ${gaugeClass}" style="width: ${Math.min(pct, 100)}%"></div>
      </div>
    `;
    return container;
  }

  private renderPortfolio(portfolio: PortfolioSummary): HTMLElement {
    const section = document.createElement('div');
    section.className = 'risk-portfolio';

    const title = document.createElement('div');
    title.className = 'risk-portfolio-title';
    title.textContent = 'Portfolio Summary';
    section.appendChild(title);

    const grid = document.createElement('div');
    grid.className = 'risk-portfolio-grid';

    const positionsEl = document.createElement('div');
    positionsEl.className = 'risk-portfolio-item';
    positionsEl.innerHTML = `<span class="risk-portfolio-label">Open Positions</span><span class="risk-portfolio-value">${portfolio.openPositionCount}</span>`;
    grid.appendChild(positionsEl);

    const notionalEl = document.createElement('div');
    notionalEl.className = 'risk-portfolio-item';
    notionalEl.innerHTML = `<span class="risk-portfolio-label">Total Notional</span><span class="risk-portfolio-value">${fmt(portfolio.total_notional)}</span>`;
    grid.appendChild(notionalEl);

    const pnlEl = document.createElement('div');
    pnlEl.className = 'risk-portfolio-item';
    const colorClass = pnlClass(portfolio.total_unrealized_pnl);
    pnlEl.innerHTML = `<span class="risk-portfolio-label">Unrealized PnL</span><span class="risk-portfolio-value ${colorClass}">${fmt(portfolio.total_unrealized_pnl)}</span>`;
    grid.appendChild(pnlEl);

    section.appendChild(grid);
    return section;
  }
}
