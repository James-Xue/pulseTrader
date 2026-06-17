# Layer 4 AI Analysis — static library
#
# Contains:
#   - twitter_feed: X (Twitter) API v2 social signal ingestion
#   - news_feed: NewsAPI / CryptoPanic article ingestion
#   - prompt_builder: system + user prompt assembly
#   - ai_client: HTTP client for OpenAI / Claude LLM backends
#   - param_advisor: delta validation + atomic StrategyParams writes

add_library(pulse_ai STATIC
    twitter_feed.cpp
    news_feed.cpp
    prompt_builder.cpp
    ai_client.cpp
    param_advisor.cpp
    ai_pipeline.cpp
)

add_library(pulse::ai ALIAS pulse_ai)

target_include_directories(pulse_ai
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(pulse_ai
    PUBLIC
        pulse::core
        pulse::logging
        pulse::strategy
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

target_compile_features(pulse_ai PUBLIC cxx_std_20)
