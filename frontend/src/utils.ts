// utils.ts — Shared formatting utilities ported from app.js

export function fmt(n: number | null | undefined, decimals = 2): string {
    if (n === null || n === undefined || isNaN(n)) return '—';
    return Number(n).toFixed(decimals);
}

export function fmtPct(n: number): string {
    return (n * 100).toFixed(2) + '%';
}

export function pnlClass(v: number): string {
    if (v > 0) return 'positive';
    if (v < 0) return 'negative';
    return '';
}

export function escHtml(s: string): string {
    const div = document.createElement('div');
    div.appendChild(document.createTextNode(s));
    return div.innerHTML;
}

export function msToTime(ms: number): string {
    if (!ms) return '—';
    return new Date(ms).toLocaleTimeString();
}

export function msToHHMM(ms: number): string {
    if (!ms) return '—';
    const d = new Date(ms);
    return ('0' + d.getHours()).slice(-2) + ':' + ('0' + d.getMinutes()).slice(-2);
}
