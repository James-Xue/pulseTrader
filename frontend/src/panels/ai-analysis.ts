import { BasePanel } from '../panels/base-panel';
import type { DataStore } from '../data-store';
import { fmt, fmtPct, escHtml, msToTime } from '../utils';
import type { AIAnalysis } from '../types';

export class AiAnalysisPanel extends BasePanel<'ai'> {
  readonly componentType = 'ai-analysis';
  readonly title = 'AI Analysis';
  readonly field = 'ai' as const;

  constructor(dataStore: DataStore) {
    super(dataStore);
  }

  protected buildDOM(): void {
    if (!this.container) return;
    this.container.innerHTML = '';
  }

  protected update(data: AIAnalysis): void {
    if (!this.container) return;

    if (!data.available) {
      this.container.innerHTML = '<p class="placeholder">Awaiting first AI cycle...</p>';
      return;
    }

    const result = data.result;
    this.container.innerHTML = '';

    // Sentiment display
    const sentimentEl = document.createElement('div');
    sentimentEl.className = 'ai-sentiment';
    const emoji = result.sentiment === 'bullish' ? '🟢' : result.sentiment === 'bearish' ? '🔴' : '⚪';
    sentimentEl.innerHTML = `<span class="ai-sentiment-emoji">${emoji}</span><span class="ai-sentiment-text">${escHtml(result.sentiment)}</span>`;
    this.container.appendChild(sentimentEl);

    // Direction bias bar
    const biasPct = ((result.direction_bias + 1) / 2) * 100;
    const biasContainer = document.createElement('div');
    biasContainer.className = 'direction-bar-container';
    biasContainer.innerHTML = `
      <div class="direction-bar-label">Direction Bias: ${fmt(result.direction_bias)}</div>
      <div class="direction-bar">
        <div class="direction-bar-marker" style="left: ${biasPct}%"></div>
      </div>
    `;
    this.container.appendChild(biasContainer);

    // Volatility badge
    const volatilityEl = document.createElement('div');
    volatilityEl.className = 'ai-volatility';
    const badgeClass = result.volatility === 'high' ? 'badge-red' : result.volatility === 'low' ? 'badge-green' : 'badge-gray';
    volatilityEl.innerHTML = `<span class="ai-volatility-label">Volatility:</span><span class="badge ${badgeClass}">${escHtml(result.volatility)}</span>`;
    this.container.appendChild(volatilityEl);

    // Confidence gauge
    const confidencePct = result.confidence * 100;
    const gaugeClass = confidencePct >= 70 ? 'gauge-fill-green' : confidencePct >= 40 ? 'gauge-fill-yellow' : 'gauge-fill-red';
    const gaugeContainer = document.createElement('div');
    gaugeContainer.className = 'gauge-container';
    gaugeContainer.innerHTML = `
      <div class="gauge-label">Confidence: ${fmtPct(confidencePct)}</div>
      <div class="gauge-track">
        <div class="gauge-fill ${gaugeClass}" style="width: ${confidencePct}%"></div>
      </div>
    `;
    this.container.appendChild(gaugeContainer);

    // Last update time
    const updateTimeEl = document.createElement('div');
    updateTimeEl.className = 'ai-last-update';
    updateTimeEl.textContent = `Last update: ${msToTime(data.last_update_ms)}`;
    this.container.appendChild(updateTimeEl);

    // Param deltas table
    if (result.param_deltas && Object.keys(result.param_deltas).length > 0) {
      const table = document.createElement('table');
      table.className = 'param-deltas-table';

      const thead = document.createElement('thead');
      thead.innerHTML = '<tr><th>Parameter</th><th>Delta</th></tr>';
      table.appendChild(thead);

      const tbody = document.createElement('tbody');
      for (const [key, delta] of Object.entries(result.param_deltas)) {
        const paramName = key.replace(/_delta$/, '').replace(/_/g, ' ');
        const colorClass = delta > 0 ? 'positive' : delta < 0 ? 'negative' : '';
        const row = document.createElement('tr');
        row.innerHTML = `<td>${escHtml(paramName)}</td><td class="${colorClass}">${fmt(delta)}</td>`;
        tbody.appendChild(row);
      }
      table.appendChild(tbody);
      this.container.appendChild(table);
    }
  }
}
