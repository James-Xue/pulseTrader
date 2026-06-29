// layout-manager.ts — Golden Layout wrapper with persistence
//
// Wraps @genesis-community/golden-layout to register panel factories,
// manage default layout, and save/restore from localStorage.

import {
    GoldenLayout,
    ContentItem,
    type LayoutConfig,
    type ComponentContainer,
    type RootItemConfig,
} from '@genesis-community/golden-layout';

import '@genesis-community/golden-layout/dist/css/goldenlayout-base.css';
import '@genesis-community/golden-layout/dist/css/themes/goldenlayout-dark-theme.css';

import type { BasePanel } from './panels/base-panel';

export class LayoutManager {
    private layout: GoldenLayout;
    private factories = new Map<string, () => BasePanel>();

    constructor(private readonly container: HTMLElement) {
        this.layout = new GoldenLayout(container);
    }

    /**
     * Register a panel factory. The factory function is called each time
     * Golden Layout creates a new instance of this component (e.g., when
     * user splits a tab).
     */
    registerFactory(componentType: string, title: string, factory: () => BasePanel): void {
        this.factories.set(componentType, factory);

        this.layout.registerComponentFactoryFunction(
            componentType,
            (container: ComponentContainer, state: unknown) => {
                const panel = factory();
                const el = container.element as HTMLElement;
                el.classList.add('panel-inner');
                panel.mount(el);

                container.stateRequestEvent = () => ({});
                container.on('beforeComponentRelease', () => { panel.destroy(); });
            }
        );
    }

    /** Load saved layout from localStorage, or use default. */
    init(): void {
        const saved = localStorage.getItem('pulsetrader-layout');
        if (saved) {
            try {
                const config = JSON.parse(saved) as LayoutConfig;
                this.layout.loadLayout(config);
                return;
            } catch {
                console.warn('[pulseTrader] Failed to load saved layout, using default');
            }
        }
        this.layout.loadLayout(this.getDefaultLayout());
    }

    /** Save current layout to localStorage. */
    save(): void {
        const config = this.layout.saveLayout();
        localStorage.setItem('pulsetrader-layout', JSON.stringify(config));
    }

    /** Reset to default layout. */
    resetLayout(): void {
        localStorage.removeItem('pulsetrader-layout');
        this.layout.loadLayout(this.getDefaultLayout());
    }

    destroy(): void {
        this.save();
        this.layout.destroy();
    }

    /** Check if a component type is currently visible in the layout. */
    isComponentVisible(componentType: string): boolean {
        return this.findComponentItem(componentType) !== null;
    }

    /** Show a component by adding it to the layout. */
    showComponent(componentType: string, title: string): void {
        if (this.isComponentVisible(componentType)) return;
        this.layout.newComponent(componentType, undefined, title);
    }

    /** Hide a component by removing it from the layout. */
    hideComponent(componentType: string): void {
        const item = this.findComponentItem(componentType);
        if (item) item.remove();
    }

    /** Get list of all currently visible component types. */
    getVisibleComponents(): string[] {
        const types: string[] = [];
        this.walkTree((item) => {
            if (ContentItem.isComponentItem(item)) {
                types.push(item.componentType as string);
            }
        });
        return types;
    }

    /** Walk the layout tree, calling visitor for each ContentItem. */
    private walkTree(visitor: (item: ContentItem) => void, node?: ContentItem): void {
        const root = node ?? this.layout.rootItem;
        if (!root) return;
        visitor(root);
        for (const child of root.contentItems) {
            this.walkTree(visitor, child);
        }
    }

    /** Find a ComponentItem by its componentType string. */
    private findComponentItem(componentType: string): ContentItem | null {
        let found: ContentItem | null = null;
        this.walkTree((item) => {
            if (!found && ContentItem.isComponentItem(item) && item.componentType === componentType) {
                found = item;
            }
        });
        return found;
    }

    private getDefaultLayout(): LayoutConfig {
        return {
            root: {
                type: 'row',
                content: [
                    {
                        type: 'column',
                        width: 60,
                        content: [
                            {
                                type: 'stack',
                                height: 65,
                                content: [
                                    { type: 'component', componentType: 'kline', title: 'K-Line Chart' },
                                ],
                            },
                            {
                                type: 'stack',
                                height: 35,
                                content: [
                                    { type: 'component', componentType: 'order-book', title: 'Order Book' },
                                ],
                            },
                        ],
                    },
                    {
                        type: 'column',
                        width: 40,
                        content: [
                            { type: 'component', componentType: 'positions', title: 'Positions', height: 20 },
                            { type: 'component', componentType: 'orders', title: 'Orders', height: 20 },
                            {
                                type: 'stack',
                                height: 20,
                                content: [
                                    { type: 'component', componentType: 'metrics', title: 'PnL & Metrics' },
                                    { type: 'component', componentType: 'risk', title: 'Risk Status' },
                                ],
                            },
                            {
                                type: 'stack',
                                height: 20,
                                content: [
                                    { type: 'component', componentType: 'strategies', title: 'Strategies' },
                                    { type: 'component', componentType: 'ai-analysis', title: 'AI Analysis' },
                                ],
                            },
                        ],
                    },
                ],
            } as RootItemConfig,
        } as LayoutConfig;
    }
}
