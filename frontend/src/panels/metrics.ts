import { BasePanel } from '../panels/base-panel';
import type { DataStore, FieldKey } from '../data-store';
import { fmt, fmtPct, pnlClass } from '../utils';
import type { Metrics } from '../types';

export class MetricsPanel extends BasePanel<'metrics'> {
  readonly componentType = 'metrics';
  readonly title = 'PnL & Metrics';
  readonly field = 'metrics' as const;

  constructor(dataStore: DataStore) {
    super(dataStore);
  }

  protected buildDOM(): void {
    if (!this.container) return;
    this.container.innerHTML = '';
  }

  protected update(data: Metrics): void {
    if (!this.container) return;

    if (!data.available) {
      this.container.innerHTML = '<p class="placeholder">Metrics not yet available</p>';
      return;
    }

    const grid = document.createElement('div');
    grid.className = 'metrics-grid';

    grid.appendChild(this.metricCard('Net PnL', fmt(data.net_pnl), pnlClass(data.net_pnl)));
    grid.appendChild(this.metricCard('Gross PnL', fmt(data.gross_pnl), pnlClass(data.gross_pnl)));
    grid.appendChild(this.metricCard('Win Rate', fmtPct(data.win_rate), ''));
    grid.appendChild(this.metricCard('Win/Loss Ratio', fmt(data.avg_win_loss_ratio), ''));

    const sharpeClass = data.sharpe_ratio >= 1 ? 'positive' : data.sharpe_ratio < 0 ? 'negative' : '';
    grid.appendChild(this.metricCard('Sharpe Ratio', fmt(data.sharpe_ratio), sharpeClass));

    const ddClass = data.max_drawdown > 3 ? 'negative' : '';
    grid.appendChild(this.metricCard('Max Drawdown', fmtPct(data.max_drawdown), ddClass));

    grid.appendChild(this.metricCard('Trade Count', String(data.trade_count), ''));

    this.container.innerHTML = '';
    this.container.appendChild(grid);
  }

  private metricCard(label: string, value: string, cssClass: string): HTMLElement {
    const card = document.createElement('div');
    card.className = 'metric-card';

    const labelEl = document.createElement('div');
    labelEl.className = 'metric-card-label';
    labelEl.textContent = label;

    const valueEl = document.createElement('div');
    valueEl.className = `metric-card-value${cssClass ? ` ${cssClass}` : ''}`;
    valueEl.textContent = value;

    card.appendChild(labelEl);
    card.appendChild(valueEl);
    return card;
  }
}
