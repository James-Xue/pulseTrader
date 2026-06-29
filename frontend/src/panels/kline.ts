// kline.ts — K-line candlestick chart panel using lightweight-charts

import { BasePanel } from './base-panel';
import type { DataStore } from '../data-store';
import type { KLine, Candle } from '../types';
import {
    createChart,
    CandlestickSeries,
    HistogramSeries,
    CrosshairMode,
    LineStyle,
    type IChartApi,
    type ISeriesApi,
    type CandlestickData,
    type HistogramData,
    type Time,
} from 'lightweight-charts';

interface ChartCandle {
    time: Time;
    open: number;
    high: number;
    low: number;
    close: number;
    volume: number;
}

export class KlinePanel extends BasePanel<'kline'> {
    readonly componentType = 'kline';
    readonly title = 'K-Line Chart';
    readonly field = 'kline' as const;

    private chart: IChartApi | null = null;
    private candleSeries: ISeriesApi<'Candlestick'> | null = null;
    private volumeSeries: ISeriesApi<'Histogram'> | null = null;
    private resizeObserver: ResizeObserver | null = null;
    private lastOpenTime = 0;

    constructor(dataStore: DataStore) {
        super(dataStore);
    }

    protected buildDOM(): void {
        if (!this.container) return;

        const chartEl = document.createElement('div');
        chartEl.className = 'kline-chart';
        chartEl.style.width = '100%';
        chartEl.style.height = '100%';
        this.container.appendChild(chartEl);

        this.chart = createChart(chartEl, {
            layout: {
                background: { color: '#1a1a2e' },
                textColor: '#a0a0c0',
                attributionLogo: false,
            },
            grid: {
                vertLines: { color: 'rgba(15,52,96,0.4)' },
                horzLines: { color: 'rgba(15,52,96,0.4)' },
            },
            crosshair: {
                mode: CrosshairMode.Normal,
            },
            timeScale: {
                borderColor: 'rgba(15,52,96,0.4)',
            },
            rightPriceScale: {
                borderColor: 'rgba(15,52,96,0.4)',
            },
        });

        this.candleSeries = this.chart.addSeries(CandlestickSeries, {
            upColor: '#4caf50',
            downColor: '#f44336',
            borderUpColor: '#4caf50',
            borderDownColor: '#f44336',
            wickUpColor: '#4caf50',
            wickDownColor: '#f44336',
        });

        this.volumeSeries = this.chart.addSeries(HistogramSeries, {
            color: '#26a69a',
            priceFormat: { type: 'volume' },
            priceScaleId: 'volume',
        });

        this.volumeSeries.priceScale().applyOptions({
            scaleMargins: { top: 0.85, bottom: 0 },
        });

        // Resize observer
        this.resizeObserver = new ResizeObserver(() => {
            if (this.chart && chartEl) {
                this.chart.applyOptions({
                    width: chartEl.clientWidth,
                    height: chartEl.clientHeight,
                });
            }
        });
        this.resizeObserver.observe(chartEl);
    }

    protected update(data: KLine): void {
        if (!data || !data.candles || data.candles.length === 0) return;
        if (!this.candleSeries || !this.volumeSeries) return;

        const chartData = this.toChartData(data.candles);
        const lastCandle = data.candles[data.candles.length - 1];

        if (lastCandle.open_time === this.lastOpenTime && chartData.length > 0) {
            // Same last candle — incremental update
            const last = chartData[chartData.length - 1];
            this.candleSeries.update(last as CandlestickData);
            this.volumeSeries.update({
                time: last.time,
                value: last.volume ?? 0,
                color: (last as CandlestickData).close >= (last as CandlestickData).open
                    ? 'rgba(76,175,80,0.5)'
                    : 'rgba(244,67,54,0.5)',
            } as HistogramData);
        } else {
            // New candle — full data replacement
            this.candleSeries.setData(chartData as CandlestickData[]);
            this.volumeSeries.setData(
                chartData.map((c) => ({
                    time: c.time,
                    value: c.volume ?? 0,
                    color: c.close >= c.open
                        ? 'rgba(76,175,80,0.5)'
                        : 'rgba(244,67,54,0.5)',
                })) as HistogramData[]
            );
            this.chart?.timeScale().fitContent();
            this.lastOpenTime = lastCandle.open_time;
        }
    }

    private toChartData(candles: Candle[]): ChartCandle[] {
        return candles.map((c) => ({
            time: Math.floor(c.open_time / 1000) as Time,
            open: c.open,
            high: c.high,
            low: c.low,
            close: c.close,
            volume: c.volume,
        }));
    }

    override destroy(): void {
        super.destroy();

        if (this.resizeObserver) {
            this.resizeObserver.disconnect();
            this.resizeObserver = null;
        }
        if (this.chart) {
            this.chart.remove();
            this.chart = null;
        }
        this.candleSeries = null;
        this.volumeSeries = null;
        this.lastOpenTime = 0;
    }
}
