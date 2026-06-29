// ws-client.ts — WebSocket client with auto-reconnect and auth
//
// Ported from app.js Connection Manager. Push-only: server sends
// DashboardSnapshot JSON, client never sends messages.

import type { DashboardSnapshot } from './types';

export type SnapshotListener = (snap: DashboardSnapshot) => void;

export class WsClient {
    private ws: WebSocket | null = null;
    private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    private reconnectDelay = 1000;
    private readonly RECONNECT_CAP = 30000;
    private listeners = new Set<SnapshotListener>();
    private statusListeners = new Set<(connected: boolean) => void>();

    constructor(private readonly url: string) {
        this.connect();
    }

    onSnapshot(listener: SnapshotListener): () => void {
        this.listeners.add(listener);
        return () => { this.listeners.delete(listener); };
    }

    onStatus(listener: (connected: boolean) => void): () => void {
        this.statusListeners.add(listener);
        return () => { this.statusListeners.delete(listener); };
    }

    private connect(): void {
        if (this.ws &&
            (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
            return;
        }

        this.ws = new WebSocket(this.url);

        this.ws.onopen = () => {
            this.reconnectDelay = 1000;
            this.notifyStatus(true);
        };

        this.ws.onmessage = (event: MessageEvent) => {
            try {
                const snap = JSON.parse(event.data as string) as DashboardSnapshot;
                for (const listener of this.listeners) {
                    listener(snap);
                }
            } catch (e) {
                console.error('[pulseTrader] Failed to parse snapshot:', e);
            }
        };

        this.ws.onclose = () => {
            this.notifyStatus(false);
            this.scheduleReconnect();
        };

        this.ws.onerror = () => {
            this.notifyStatus(false);
        };
    }

    private scheduleReconnect(): void {
        if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            this.connect();
        }, this.reconnectDelay);
        this.reconnectDelay = Math.min(this.reconnectDelay * 2, this.RECONNECT_CAP);
    }

    private notifyStatus(connected: boolean): void {
        for (const listener of this.statusListeners) {
            listener(connected);
        }
    }

    destroy(): void {
        if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
        if (this.ws) this.ws.close();
        this.listeners.clear();
        this.statusListeners.clear();
    }
}
