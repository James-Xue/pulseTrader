import { BasePanel } from '../panels/base-panel';
import type { DataStore } from '../data-store';
import { escHtml } from '../utils';
import type { Strategies, Strategy } from '../types';

export class StrategiesPanel extends BasePanel<'strategies'> {
  readonly componentType = 'strategies';
  readonly title = 'Strategies';
  readonly field = 'strategies' as const;

  constructor(dataStore: DataStore) {
    super(dataStore);
  }

  protected buildDOM(): void {
    if (!this.container) return;
    this.container.innerHTML = '';
  }

  protected update(data: Strategies): void {
    if (!this.container) return;

    if (!data.strategies || data.strategies.length === 0) {
      this.container.innerHTML = '<p class="placeholder">No strategies registered</p>';
      return;
    }

    this.container.innerHTML = '';

    for (const strategy of data.strategies) {
      const card = this.renderStrategyCard(strategy);
      this.container.appendChild(card);
    }
  }

  private renderStrategyCard(strategy: Strategy): HTMLElement {
    const card = document.createElement('div');
    card.className = 'strategy-card';

    // Header: name + running badge
    const header = document.createElement('div');
    header.className = 'strategy-card-header';
    const nameEl = document.createElement('span');
    nameEl.className = 'strategy-card-name';
    nameEl.textContent = strategy.name;
    header.appendChild(nameEl);

    const runningBadge = document.createElement('span');
    if (strategy.running) {
      runningBadge.className = 'badge badge-green pulse';
      runningBadge.textContent = 'Running';
    } else {
      runningBadge.className = 'badge badge-gray';
      runningBadge.textContent = 'Stopped';
    }
    header.appendChild(runningBadge);
    card.appendChild(header);

    // Meta: symbol, poll interval, enabled badge
    const meta = document.createElement('div');
    meta.className = 'strategy-card-meta';

    const symbolEl = document.createElement('span');
    symbolEl.className = 'strategy-card-symbol';
    symbolEl.textContent = strategy.symbol;
    meta.appendChild(symbolEl);

    const intervalEl = document.createElement('span');
    intervalEl.className = 'strategy-card-interval';
    intervalEl.textContent = `${strategy.poll_interval_ms}ms`;
    meta.appendChild(intervalEl);

    const enabledBadge = document.createElement('span');
    if (strategy.enabled) {
      enabledBadge.className = 'badge badge-green';
      enabledBadge.textContent = 'Enabled';
    } else {
      enabledBadge.className = 'badge badge-gray';
      enabledBadge.textContent = 'Disabled';
    }
    meta.appendChild(enabledBadge);
    card.appendChild(meta);

    // Footer: strategy id
    const footer = document.createElement('div');
    footer.className = 'strategy-card-footer';
    const idEl = document.createElement('span');
    idEl.className = 'strategy-card-id';
    idEl.textContent = strategy.id;
    footer.appendChild(idEl);
    card.appendChild(footer);

    return card;
  }
}
