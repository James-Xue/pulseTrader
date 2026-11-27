add_subdirectory(unit)

# Cross-layer flows (2026-11-27 audit P5). The paper flow needs the SQLite
# trade recorder (history + PnL rows), so the directory is SQLite-only.
if(PULSE_ENABLE_SQLITE)
    add_subdirectory(integration)
endif()
