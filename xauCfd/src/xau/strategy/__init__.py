"""strategy — signal generators.

Submodules:
    trend        — EMA(30) vs EMA(100) over recent bars → UP/DOWN/RANGE
    sr_reversion — for each S/R level, plan a BUY_LIMIT or SELL_LIMIT
                   offset 3-5 pts past the level, filtered by trend
"""