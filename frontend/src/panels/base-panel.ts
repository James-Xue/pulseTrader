// base-panel.ts — Abstract base class for all dashboard panels

import type { DataStore, FieldKey } from '../data-store';

export abstract class BasePanel<K extends FieldKey = FieldKey> {
    protected container: HTMLElement | null = null;
    protected unsubscribes: (() => void)[] = [];

    constructor(protected readonly dataStore: DataStore) {}

    abstract readonly componentType: string;
    abstract readonly title: string;
    abstract readonly field: K;

    /** Mount the panel into the given container element. */
    mount(container: HTMLElement): void {
        this.container = container;
        this.buildDOM();
        this.subscribeData();
    }

    /** Build the initial DOM structure. Override in subclasses. */
    protected abstract buildDOM(): void;

    /** Subscribe to data store updates. */
    protected subscribeData(): void {
        const unsub = this.dataStore.subscribe(this.field, (data) => {
            this.update(data);
        });
        this.unsubscribes.push(unsub);
    }

    /** Handle data update. Override in subclasses. */
    protected abstract update(data: any): void;

    /** Cleanup when panel is destroyed. */
    destroy(): void {
        for (const unsub of this.unsubscribes) unsub();
        this.unsubscribes = [];
        if (this.container) this.container.innerHTML = '';
        this.container = null;
    }
}
