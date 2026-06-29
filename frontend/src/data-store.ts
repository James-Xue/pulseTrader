// data-store.ts — Central data store with field-level subscription
//
// Receives DashboardSnapshot from WebSocket and dispatches individual
// fields to subscribed panels. This avoids each panel parsing the
// full snapshot.

import type { DashboardSnapshot } from './types';

export type FieldKey = keyof DashboardSnapshot;
export type FieldListener<K extends FieldKey> = (data: DashboardSnapshot[K]) => void;

export class DataStore {
    private snapshot: DashboardSnapshot | null = null;
    private listeners = new Map<string, Set<FieldListener<FieldKey>>>();
    private connectionStatus = false;
    private statusListeners = new Set<(connected: boolean) => void>();

    update(snapshot: DashboardSnapshot): void {
        this.snapshot = snapshot;
        this.dispatch();
    }

    setConnectionStatus(connected: boolean): void {
        this.connectionStatus = connected;
        for (const listener of this.statusListeners) {
            listener(connected);
        }
    }

    subscribe<K extends FieldKey>(field: K, listener: FieldListener<K>): () => void {
        const key = field as string;
        if (!this.listeners.has(key)) {
            this.listeners.set(key, new Set());
        }
        this.listeners.get(key)!.add(listener as FieldListener<FieldKey>);

        // If we already have data, deliver immediately
        if (this.snapshot) {
            listener(this.snapshot[field]);
        }

        return () => {
            this.listeners.get(key)?.delete(listener as FieldListener<FieldKey>);
        };
    }

    onConnectionStatus(listener: (connected: boolean) => void): () => void {
        this.statusListeners.add(listener);
        listener(this.connectionStatus);
        return () => { this.statusListeners.delete(listener); };
    }

    getSnapshot(): DashboardSnapshot | null {
        return this.snapshot;
    }

    private dispatch(): void {
        if (!this.snapshot) return;

        for (const [field, listeners] of this.listeners) {
            const data = this.snapshot[field as FieldKey];
            for (const listener of listeners) {
                listener(data);
            }
        }
    }
}
