"""infra — config, logging, time.

Submodules:
    config    — TOML loader with `from_env:` placeholders (stdlib tomllib)
    logging   — structlog setup, JSON + console sinks
    clock     — monotonic + wall-clock helpers, injectable for tests
"""