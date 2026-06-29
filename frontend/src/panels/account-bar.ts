// account-bar.ts — Account balance bar (fixed in header area, not a GL panel)
//
// Renders futures + spot account balances above the Golden Layout container.

import type { DataStore } from '../data-store';
import type { Account } from '../types';
import { fmt, pnlClass } from '../utils';

export class AccountBar {
    private el: HTMLElement;

    constructor(private readonly dataStore: DataStore) {
        this.el = document.getElementById('account-bar')!;
        this.dataStore.subscribe('account', (data) => this.update(data));
    }

    private update(acct: Account): void {
        const hasFutures = acct.available;
        const hasSpot = acct.spot_available;

        if (!hasFutures && !hasSpot) {
            this.el.classList.add('hidden');
            return;
        }

        this.el.classList.remove('hidden');

        if (hasFutures) {
            const cur = acct.currency || 'USDT';
            this.setText('account-total', fmt(acct.total) + ' ' + cur);
            this.setText('account-available', fmt(acct.available_balance) + ' ' + cur);

            const pnlEl = document.getElementById('account-pnl');
            if (pnlEl) {
                pnlEl.textContent = (acct.unrealised_pnl >= 0 ? '+' : '') +
                    fmt(acct.unrealised_pnl) + ' ' + cur;
                pnlEl.className = 'account-value ' + pnlClass(acct.unrealised_pnl);
            }

            const marginUsed = (acct.position_margin || 0) + (acct.order_margin || 0);
            this.setText('account-margin', fmt(marginUsed) + ' ' + cur);
        }

        if (hasSpot) {
            const scur = acct.spot_currency || 'USDT';
            this.setText('account-spot-total', fmt(acct.spot_total) + ' ' + scur);
            this.setText('account-spot-available', fmt(acct.spot_available_balance) + ' ' + scur);
        }
    }

    private setText(id: string, text: string): void {
        const el = document.getElementById(id);
        if (el) el.textContent = text;
    }
}
