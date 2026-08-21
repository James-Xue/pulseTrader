# Layer 4 AI Analysis — static library
#
# Contains:
#   - twitter_feed: X (Twitter) API v2 social signal ingestion
#   - news_feed: NewsAPI / CryptoPanic article ingestion
#   - prompt_builder: system + user prompt assembly
#   - ai_client: HTTP client for OpenAI / Claude LLM backends
#   - param_advisor: delta validation + atomic StrategyParams writes

add_library(pulse_ai STATIC
    TwitterFeed.cpp
    NewsFeed.cpp
    PromptBuilder.cpp
    AIClient.cpp
    ParamAdvisor.cpp
    AiPipeline.cpp
    EngineSnapshotCollector.cpp
)

add_library(pulse::ai ALIAS pulse_ai)

target_include_directories(pulse_ai
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(pulse_ai
    PUBLIC
        pulse::core
        pulse::logging
        pulse::strategy
        pulse::market
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

# Per-strategy performance feedback (trades DB) is only available in
# SQLite-enabled builds; the collector guards the call site with #ifdef.
if(PULSE_ENABLE_SQLITE)
    target_link_libraries(pulse_ai
        PRIVATE
            pulse::trade_recorder
    )
    target_compile_definitions(pulse_ai PRIVATE PULSE_ENABLE_SQLITE=1)
endif()

target_compile_features(pulse_ai PUBLIC cxx_std_20)
