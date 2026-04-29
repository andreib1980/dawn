# =============================================================================
# Dawn Test Suite
# =============================================================================
# Build all tests: make tests
# Build individual test: make test_<name>
#
# Note: This file is included from the main CMakeLists.txt with EXCLUDE_FROM_ALL,
# so tests are not built by default.

set(CMAKE_C_STANDARD 11)

# Unity test framework (vendored, matches STAT/ECHO repos)
# NOTE: DAWN uses EXCLUDE_FROM_ALL for tests (unlike ECHO/STAT which build tests
# by default) because the Jetson target has limited resources. This means ctest
# is a test-runner only — `make tests-ci` must be run first to build the binaries.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# Source root directory (parent of tests/)
set(SRC_ROOT ${CMAKE_SOURCE_DIR})

# Include directories (inherited from parent, but be explicit for clarity)
include_directories(${SRC_ROOT}/include)

# =============================================================================
# Test Executables
# =============================================================================

# Test: SSE Parser
add_executable(test_sse_parser
               test_sse_parser.c
               ${SRC_ROOT}/src/llm/sse_parser.c
)

target_link_libraries(test_sse_parser
                      dawn_common
                      unity
                      ${JSONC_LIBRARIES})
add_test(NAME test_sse_parser COMMAND test_sse_parser)

# Test: Sentence Buffer
add_executable(test_sentence_buffer
               test_sentence_buffer.c
)

target_link_libraries(test_sentence_buffer
                      dawn_common
                      unity
                      ${JSONC_LIBRARIES})
add_test(NAME test_sentence_buffer COMMAND test_sentence_buffer)

# Test: End-to-End Streaming
add_executable(test_streaming
               test_streaming.c
               ${SRC_ROOT}/src/llm/llm_interface.c
               ${SRC_ROOT}/src/llm/llm_openai.c
               ${SRC_ROOT}/src/llm/llm_claude.c
               ${SRC_ROOT}/src/llm/llm_streaming.c
               ${SRC_ROOT}/src/llm/sse_parser.c
               ${SRC_ROOT}/src/config/config_parser.c
               ${SRC_ROOT}/src/config/config_defaults.c
               ${SRC_ROOT}/src/config/config_env.c
               ${SRC_ROOT}/src/tools/toml.c
               ${SRC_ROOT}/src/ui/metrics.c
)

target_link_libraries(test_streaming
                      dawn_common
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      OpenSSL::SSL
                      OpenSSL::Crypto)
add_test(NAME test_streaming COMMAND test_streaming)

# Test: ASR Benchmark (requires both Vosk and Whisper)
add_executable(asr_benchmark
               asr_benchmark.c
               ${SRC_ROOT}/src/asr/asr_interface.c
               ${SRC_ROOT}/src/asr/asr_vosk.c
)

target_include_directories(asr_benchmark PRIVATE
                           ${SRC_ROOT}/whisper.cpp/include)

target_link_libraries(asr_benchmark
                      dawn_common_asr
                      vosk
                      whisper
                      sndfile
                      ${JSONC_LIBRARIES}
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES})
add_test(NAME asr_benchmark COMMAND asr_benchmark)

# Test: VAD Model Comparison
add_executable(test_vad_models test_vad_models.c)
target_include_directories(test_vad_models PRIVATE /usr/local/include/onnxruntime)
target_link_libraries(test_vad_models onnxruntime m)
add_test(NAME test_vad_models COMMAND test_vad_models)

# Test: VAD API Unit Test
add_executable(test_vad_api
               test_vad_api.c
)

target_include_directories(test_vad_api PRIVATE /usr/local/include/onnxruntime)

target_link_libraries(test_vad_api
                      dawn_common_vad
                      onnxruntime
                      m
                      ${JSONC_LIBRARIES})
add_test(NAME test_vad_api COMMAND test_vad_api)

# Test: ASR Concurrent Context
# Verifies multi-context support for multi-client architecture
add_executable(test_asr_concurrent
               test_asr_concurrent.c
               ${SRC_ROOT}/src/asr/asr_interface.c
               ${SRC_ROOT}/src/ui/metrics.c
)

target_include_directories(test_asr_concurrent PRIVATE
                           ${SRC_ROOT}/whisper.cpp/include)

target_link_libraries(test_asr_concurrent
                      dawn_common_asr
                      whisper
                      pthread
                      m
                      ${JSONC_LIBRARIES}
                      ${CUDA_LIBRARIES}
                      ${CUDART_LIBRARIES}
                      ${CUSPARSE_LIBRARIES}
                      ${CUBLAS_LIBRARIES}
                      ${CUSOLVER_LIBRARIES}
                      ${CURAND_LIBRARIES})
add_test(NAME test_asr_concurrent COMMAND test_asr_concurrent)

# Test: Per-Session LLM Commands
# Tests thread-local storage mechanism for command context (session pointer passing)
# This is a self-contained test that validates TLS without needing full infrastructure
add_executable(test_session_commands
               test_session_commands.c)

target_link_libraries(test_session_commands
                      unity
                      pthread)
add_test(NAME test_session_commands COMMAND test_session_commands)

# Test: TF-IDF Summarizer Quality
# Fetches articles from diverse outlets and analyzes summarizer output quality
add_executable(test_summarizer_quality
               test_summarizer_quality.c
               ${SRC_ROOT}/src/tools/tfidf_summarizer.c
               ${SRC_ROOT}/src/tools/url_fetcher.c
               ${SRC_ROOT}/src/tools/html_parser.c
               ${SRC_ROOT}/src/config/config_parser.c
               ${SRC_ROOT}/src/config/config_defaults.c
               ${SRC_ROOT}/src/config/config_env.c
               ${SRC_ROOT}/src/tools/toml.c
)

target_link_libraries(test_summarizer_quality
                      dawn_common
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES}
                      m)
add_test(NAME test_summarizer_quality COMMAND test_summarizer_quality)

# Test: Scheduler DB Unit Test
add_executable(test_scheduler
               test_scheduler.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/core/scheduler_db.c)

target_link_libraries(test_scheduler
                      dawn_common
                      unity
                      sqlite3
                      pthread
                      ${JSONC_LIBRARIES})
add_test(NAME test_scheduler COMMAND test_scheduler)

# Test: Music DB Unit Test
# Tests search, dedup, browse, source abstraction, and stale deletion
add_executable(test_music_db
               test_music_db.c
               ${SRC_ROOT}/src/audio/music_db.c
               ${SRC_ROOT}/src/audio/music_source.c
               ${SRC_ROOT}/src/core/path_utils.c)

target_link_libraries(test_music_db
                      dawn_common
                      unity
                      sqlite3
                      pthread)
add_test(NAME test_music_db COMMAND test_music_db)

# Test: Memory Embeddings Math Utilities
# Tests cosine similarity, L2 norm, and hybrid score logic (no ONNX/DB needed)
add_executable(test_memory_embeddings
               test_memory_embeddings.c
               test_memory_embeddings_stub.c
               ${SRC_ROOT}/src/memory/memory_embeddings.c
               ${SRC_ROOT}/src/core/time_query_parser.c
               ${SRC_ROOT}/src/core/embedding_engine.c)

target_link_libraries(test_memory_embeddings
                      dawn_common
                      unity
                      sqlite3
                      pthread
                      m)
add_test(NAME test_memory_embeddings COMMAND test_memory_embeddings)

# Test: Context Compaction (LCM Phase 1)
# Tests deterministic compaction, target calculation, and token range estimation.
# Uses standalone impl file — no dependency on full llm_context.c.
add_executable(test_context_compaction
               test_context_compaction.c
               test_context_compaction_impl.c)

target_link_libraries(test_context_compaction unity ${JSONC_LIBRARIES})
add_test(NAME test_context_compaction COMMAND test_context_compaction)

# Test: Time Query Parser (#3-A)
# Pure module: parses natural-language temporal expressions in queries.
add_executable(test_time_query_parser
               test_time_query_parser.c
               ${SRC_ROOT}/src/core/time_query_parser.c)

target_link_libraries(test_time_query_parser unity m)
add_test(NAME test_time_query_parser COMMAND test_time_query_parser)

# Test: Temporal Relations (v33)
# Tests memory_db_relation_supersede auto-close behavior + as-of queries.
# Links memory_db.c against an in-memory SQLite, stubs out auth_db and config.
add_executable(test_temporal_relations
               test_temporal_relations.c
               test_temporal_relations_stub.c
               ${SRC_ROOT}/src/memory/memory_db.c
               ${SRC_ROOT}/src/memory/memory_similarity.c)

target_link_libraries(test_temporal_relations
                      dawn_common
                      unity
                      sqlite3
                      pthread
                      m)
add_test(NAME test_temporal_relations COMMAND test_temporal_relations)

# Test: Document Extract
# Tests extension validation, Content-Type mapping, plain text extraction, error codes
add_executable(test_document_extract
               test_document_extract.c
               ${SRC_ROOT}/src/tools/document_extract.c
               ${SRC_ROOT}/src/tools/html_parser.c)

target_include_directories(test_document_extract PRIVATE ${SRC_ROOT}/include)
target_link_libraries(test_document_extract dawn_common unity)

# Conditionally link document extraction libraries for the test
if(MUPDF_FOUND)
    target_link_libraries(test_document_extract
        ${MUPDF_LIB} ${MUPDF_THIRD_LIB}
        mujs gumbo openjp2 jbig2dec jpeg
        ${HARFBUZZ_LIB} ${FREETYPE2_LIBRARIES} z m)
    target_compile_definitions(test_document_extract PRIVATE HAVE_MUPDF)
else()
    target_link_libraries(test_document_extract m)
endif()
if(LIBZIP_FOUND AND LIBXML2_FOUND)
    target_link_libraries(test_document_extract ${LIBZIP_LIBRARIES} ${LIBXML2_LIBRARIES})
    target_compile_definitions(test_document_extract PRIVATE HAVE_LIBZIP HAVE_LIBXML2)
endif()
add_test(NAME test_document_extract COMMAND test_document_extract)

# Test: Document Chunker
# Tests paragraph splitting, sentence splitting, overlap, merging, edge cases
add_executable(test_document_chunker
               test_document_chunker.c
               ${SRC_ROOT}/src/tools/document_chunker.c)

target_link_libraries(test_document_chunker unity m)
add_test(NAME test_document_chunker COMMAND test_document_chunker)

# Test: Document DB CRUD
# Tests create, get, list, delete, global toggle, find_by_name, chunk ops
add_executable(test_document_db
               test_document_db.c
               test_document_db_stub.c
               ${SRC_ROOT}/src/tools/document_db.c)

target_link_libraries(test_document_db
                      dawn_common
                      unity
                      sqlite3
                      pthread
                      ${JSONC_LIBRARIES})
add_test(NAME test_document_db COMMAND test_document_db)

# Test: Embedding Engine Math
# Tests L2 norm, cosine similarity (no model/provider needed)
add_executable(test_embedding_engine
               test_embedding_engine.c
               test_embedding_engine_stub.c
               ${SRC_ROOT}/src/core/embedding_engine.c)

target_link_libraries(test_embedding_engine
                      dawn_common
                      unity
                      pthread
                      m)
add_test(NAME test_embedding_engine COMMAND test_embedding_engine)

# Test: Plan Executor
# Tests variable store, conditions, interpolation, parsing, execution, safety limits
add_executable(test_plan_executor
               test_plan_executor.c
               test_plan_executor_stub.c
               ${SRC_ROOT}/src/tools/plan_executor.c)

target_link_libraries(test_plan_executor
                      dawn_common
                      unity
                      ${JSONC_LIBRARIES}
                      pthread)
add_test(NAME test_plan_executor COMMAND test_plan_executor)

# Test: Wake Word Matching
# Tests normalization, wake word detection, command extraction, goodbye/cancel/ignore
add_executable(test_wake_word
               test_wake_word.c
               ${SRC_ROOT}/src/core/wake_word.c)

target_link_libraries(test_wake_word
                      dawn_common
                      unity)
add_test(NAME test_wake_word COMMAND test_wake_word)

# Test: Instruction Loader
# Tests file loading, path traversal sanitization, module concatenation, edge cases
add_executable(test_instruction_loader
               test_instruction_loader.c
               ${SRC_ROOT}/src/tools/instruction_loader.c)

target_link_libraries(test_instruction_loader
                      dawn_common
                      unity)
add_test(NAME test_instruction_loader COMMAND test_instruction_loader)

# =============================================================================
# Benchmark: Retrieval Quality (LongMemEval, LoCoMo, ConvoMem)
# =============================================================================
# Standalone binary exercising DAWN's embedding engine + document DB search.
# Driven by benchmarks/run_benchmark.py via JSON-lines on stdin/stdout.
# Requires ONNX Runtime (or Ollama/OpenAI endpoint) for real embeddings.
add_executable(bench_retrieval
               ${SRC_ROOT}/benchmarks/bench_retrieval.c
               ${SRC_ROOT}/benchmarks/bench_retrieval_stub.c
               ${SRC_ROOT}/src/tools/document_db.c
               ${SRC_ROOT}/src/core/time_query_parser.c
               ${SRC_ROOT}/src/core/embedding_engine.c
               ${SRC_ROOT}/src/memory/memory_embed_onnx.c
               ${SRC_ROOT}/src/memory/memory_embed_ollama.c
               ${SRC_ROOT}/src/memory/memory_embed_openai.c)

target_include_directories(bench_retrieval PRIVATE
                           /usr/local/include/onnxruntime)

target_link_libraries(bench_retrieval
                      dawn_common
                      sqlite3
                      pthread
                      m
                      onnxruntime
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES})

# Contradiction detection experiment.  Tests whether cosine similarity +
# Jaccard divergence can separate contradictory facts from paraphrases.
add_executable(bench_contradiction
               ${SRC_ROOT}/benchmarks/bench_contradiction.c
               ${SRC_ROOT}/benchmarks/bench_contradiction_stub.c
               ${SRC_ROOT}/src/core/embedding_engine.c
               ${SRC_ROOT}/src/memory/memory_embed_onnx.c
               ${SRC_ROOT}/src/memory/memory_embed_ollama.c
               ${SRC_ROOT}/src/memory/memory_embed_openai.c
               ${SRC_ROOT}/src/memory/memory_similarity.c)

target_include_directories(bench_contradiction PRIVATE
                           /usr/local/include/onnxruntime)

target_link_libraries(bench_contradiction
                      dawn_common
                      pthread
                      m
                      onnxruntime
                      ${CURL_LIBRARIES}
                      ${JSONC_LIBRARIES})

# Test: Relation-driven fact supersede
add_executable(test_relation_supersede
               test_relation_supersede.c
               test_relation_supersede_stub.c
               ${SRC_ROOT}/src/memory/memory_db.c
               ${SRC_ROOT}/src/memory/memory_similarity.c)

target_link_libraries(test_relation_supersede
                      dawn_common
                      unity
                      sqlite3
                      pthread
                      m)
add_test(NAME test_relation_supersede COMMAND test_relation_supersede)

# Test: Phone DB Unit Test
add_executable(test_phone_db
               test_phone_db.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/tools/phone_db.c)

target_link_libraries(test_phone_db
                      dawn_common
                      unity
                      sqlite3
                      pthread)
add_test(NAME test_phone_db COMMAND test_phone_db)

# Test: Missed Notifications DB Unit Test
add_executable(test_missed_notifications_db
               test_missed_notifications_db.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/core/missed_notifications_db.c)

target_link_libraries(test_missed_notifications_db
                      dawn_common
                      unity
                      sqlite3
                      pthread)
add_test(NAME test_missed_notifications_db COMMAND test_missed_notifications_db)

# Test: Memory Injection Filter
# Pure module: blocked pattern detection, normalization, ReAct co-occurrence.
add_executable(test_memory_filter
               test_memory_filter.c
               ${SRC_ROOT}/src/memory/memory_filter.c)

target_link_libraries(test_memory_filter
                      dawn_common
                      unity)
add_test(NAME test_memory_filter COMMAND test_memory_filter)

# Test: Word to Number
# Tests English text-to-number conversion (units, teens, tens, magnitudes, decimals)
add_executable(test_word_to_number
               test_word_to_number.c
               ${SRC_ROOT}/src/word_to_number.c)

target_link_libraries(test_word_to_number unity m)
add_test(NAME test_word_to_number COMMAND test_word_to_number)

# Test: Text Filter
# Tests <command> tag stripping, nesting, streaming chunks, buffer overflow
add_executable(test_text_filter
               test_text_filter.c
               ${SRC_ROOT}/src/core/text_filter.c)

target_link_libraries(test_text_filter unity)
add_test(NAME test_text_filter COMMAND test_text_filter)

# Test: Memory Similarity
# Tests text normalization, FNV-1a hashing, Jaccard similarity, dedup detection
add_executable(test_memory_similarity
               test_memory_similarity.c
               ${SRC_ROOT}/src/memory/memory_similarity.c)

target_link_libraries(test_memory_similarity unity m)
add_test(NAME test_memory_similarity COMMAND test_memory_similarity)

# Test: Rate Limiter
# Tests rate limiting, LRU eviction, IPv6 normalization
add_executable(test_rate_limiter
               test_rate_limiter.c
               ${SRC_ROOT}/src/core/rate_limiter.c)

target_link_libraries(test_rate_limiter unity pthread)
add_test(NAME test_rate_limiter COMMAND test_rate_limiter)

# Test: Crypto Store
# Tests libsodium encrypt/decrypt roundtrip, corrupt data, buffer checks
add_executable(test_crypto_store
               test_crypto_store.c
               test_crypto_store_stub.c
               ${SRC_ROOT}/src/core/crypto_store.c)

target_link_libraries(test_crypto_store dawn_common unity sodium pthread)
add_test(NAME test_crypto_store COMMAND test_crypto_store)

# Test: Input Queue
# Tests FIFO push/pop, full queue eviction, clear, source names
add_executable(test_input_queue
               test_input_queue.c
               ${SRC_ROOT}/src/input_queue.c)

target_link_libraries(test_input_queue dawn_common unity pthread)
add_test(NAME test_input_queue COMMAND test_input_queue)

# Test: Config Validate
# Tests range/enum/dependency validation of dawn_config_t
add_executable(test_config_validate
               test_config_validate.c
               ${SRC_ROOT}/src/config/config_validate.c)

target_link_libraries(test_config_validate unity)
add_test(NAME test_config_validate COMMAND test_config_validate)

# Test: Calculator
# Tests math evaluation (tinyexpr), unit/base conversion, random
add_executable(test_calculator
               test_calculator.c
               ${SRC_ROOT}/src/tools/calculator.c
               ${SRC_ROOT}/src/tools/tinyexpr.c)

target_link_libraries(test_calculator dawn_common unity m pthread)
add_test(NAME test_calculator COMMAND test_calculator)

# Test: Contacts DB
# Tests CRUD, search, field filtering, user isolation
add_executable(test_contacts_db
               test_contacts_db.c
               test_contacts_db_stub.c
               ${SRC_ROOT}/src/memory/contacts_db.c)

target_link_libraries(test_contacts_db dawn_common unity sqlite3 pthread)
add_test(NAME test_contacts_db COMMAND test_contacts_db)

# Test: Calendar DB
# Tests account/calendar/event CRUD, cascade delete
add_executable(test_calendar_db
               test_calendar_db.c
               ${SRC_ROOT}/src/tools/calendar_db.c
               ${SRC_ROOT}/src/auth/auth_db_core.c
               ${SRC_ROOT}/src/auth/auth_db_user.c
               ${SRC_ROOT}/src/auth/auth_db_session.c
               ${SRC_ROOT}/src/auth/auth_db_conv.c
               ${SRC_ROOT}/src/auth/auth_db_settings.c
               ${SRC_ROOT}/src/auth/auth_db_satellite.c
               ${SRC_ROOT}/src/auth/auth_db_metrics.c
               ${SRC_ROOT}/src/auth/auth_db_maint.c
               ${SRC_ROOT}/src/auth/auth_db_audit.c
               ${SRC_ROOT}/src/auth/auth_db_rate_limit.c
               ${SRC_ROOT}/src/core/path_utils.c)

target_link_libraries(test_calendar_db dawn_common unity sqlite3 pthread m)
add_test(NAME test_calendar_db COMMAND test_calendar_db)

# Test: Auth DB
# Tests user CRUD, sessions, conversations, settings
add_executable(test_auth_db
               test_auth_db.c
               ${SRC_ROOT}/src/auth/auth_db_core.c
               ${SRC_ROOT}/src/auth/auth_db_user.c
               ${SRC_ROOT}/src/auth/auth_db_session.c
               ${SRC_ROOT}/src/auth/auth_db_conv.c
               ${SRC_ROOT}/src/auth/auth_db_settings.c
               ${SRC_ROOT}/src/auth/auth_db_satellite.c
               ${SRC_ROOT}/src/auth/auth_db_metrics.c
               ${SRC_ROOT}/src/auth/auth_db_maint.c
               ${SRC_ROOT}/src/auth/auth_db_audit.c
               ${SRC_ROOT}/src/auth/auth_db_rate_limit.c
               ${SRC_ROOT}/src/core/path_utils.c)

target_link_libraries(test_auth_db dawn_common unity sqlite3 pthread m)
add_test(NAME test_auth_db COMMAND test_auth_db)

# Test: Tool Registry
# Tests register, lookup, alias, capability, count, lock, variations
add_executable(test_tool_registry
               test_tool_registry.c
               test_tool_registry_stub.c
               ${SRC_ROOT}/src/tools/tool_registry.c)

target_link_libraries(test_tool_registry dawn_common unity pthread ${JSONC_LIBRARIES})
add_test(NAME test_tool_registry COMMAND test_tool_registry)

# Test: Device Types
# Tests pattern matching for boolean/analog/getter/music/trigger/passphrase device types
add_executable(test_device_types
               test_device_types.c
               ${SRC_ROOT}/src/core/device_types.c)

target_link_libraries(test_device_types dawn_common unity)
add_test(NAME test_device_types COMMAND test_device_types)

# Test: HTML Parser
# Tests HTML-to-Markdown conversion, entity decoding, link resolution, plain-text mode
add_executable(test_html_parser
               test_html_parser.c
               ${SRC_ROOT}/src/tools/html_parser.c)

target_link_libraries(test_html_parser dawn_common unity)
add_test(NAME test_html_parser COMMAND test_html_parser)

# Test: TTS Preprocessing
# Tests preprocess_text_for_tts_c() — emoji/asterisk stripping, em-dash, currency,
# temperature, state/day/month abbreviation expansion. Compiles tts_preprocessing.cpp
# directly to avoid dawn_common_tts (which requires Piper/ONNX).
add_executable(test_tts_preprocessing
               test_tts_preprocessing.c
               ${SRC_ROOT}/common/src/tts/tts_preprocessing.cpp)

target_link_libraries(test_tts_preprocessing unity)
add_test(NAME test_tts_preprocessing COMMAND test_tts_preprocessing)

# Test: Email DB
# Tests email account CRUD, list, user isolation, flags, OAuth variant
add_executable(test_email_db
               test_email_db.c
               test_email_db_stub.c
               ${SRC_ROOT}/src/tools/email_db.c
               ${SRC_ROOT}/src/auth/auth_db_core.c
               ${SRC_ROOT}/src/auth/auth_db_user.c
               ${SRC_ROOT}/src/auth/auth_db_session.c
               ${SRC_ROOT}/src/auth/auth_db_conv.c
               ${SRC_ROOT}/src/auth/auth_db_settings.c
               ${SRC_ROOT}/src/auth/auth_db_satellite.c
               ${SRC_ROOT}/src/auth/auth_db_metrics.c
               ${SRC_ROOT}/src/auth/auth_db_maint.c
               ${SRC_ROOT}/src/auth/auth_db_audit.c
               ${SRC_ROOT}/src/auth/auth_db_rate_limit.c
               ${SRC_ROOT}/src/core/path_utils.c)

target_link_libraries(test_email_db dawn_common unity sqlite3 pthread m)
add_test(NAME test_email_db COMMAND test_email_db)

# =============================================================================
# CTest Labels
# =============================================================================
# CI-safe tests: no ONNX, CUDA, hardware, or external API dependencies
set_tests_properties(
   test_scheduler test_music_db test_document_chunker test_document_db
   test_embedding_engine test_memory_embeddings test_memory_filter
   test_plan_executor test_sse_parser test_sentence_buffer
   test_session_commands test_wake_word test_instruction_loader
   test_document_extract test_phone_db test_missed_notifications_db
   test_temporal_relations test_relation_supersede
   test_time_query_parser test_context_compaction
   test_word_to_number test_text_filter test_memory_similarity
   test_rate_limiter test_crypto_store
   test_input_queue test_config_validate test_calculator
   test_contacts_db test_calendar_db test_auth_db test_tool_registry
   test_device_types test_html_parser test_tts_preprocessing test_email_db
   PROPERTIES LABELS "ci")

# Hardware-dependent tests: require ONNX, CUDA, or external APIs
set_tests_properties(
   test_streaming asr_benchmark test_vad_models test_vad_api
   test_asr_concurrent test_summarizer_quality
   PROPERTIES LABELS "hardware")

# =============================================================================
# Aggregate "tests" target
# =============================================================================
# This creates a target that builds all tests when you run "make tests"
# CI-safe tests: no ONNX, CUDA, hardware, or external API dependencies
add_custom_target(tests-ci
                  DEPENDS test_scheduler
                          test_music_db
                          test_document_chunker
                          test_document_db
                          test_embedding_engine
                          test_memory_embeddings
                          test_memory_filter
                          test_plan_executor
                          test_sse_parser
                          test_sentence_buffer
                          test_session_commands
                          test_wake_word
                          test_instruction_loader
                          test_document_extract
                          test_phone_db
                          test_missed_notifications_db
                          test_temporal_relations
                          test_relation_supersede
                          test_time_query_parser
                          test_context_compaction
                          test_word_to_number
                          test_text_filter
                          test_memory_similarity
                          test_rate_limiter
                          test_crypto_store
                          test_input_queue
                          test_config_validate
                          test_calculator
                          test_contacts_db
                          test_calendar_db
                          test_auth_db
                          test_tool_registry
                          test_device_types
                          test_html_parser
                          test_tts_preprocessing
                          test_email_db
                  COMMENT "Building CI-safe unit tests")

add_custom_target(tests
                  DEPENDS bench_retrieval
                          test_instruction_loader
                          test_wake_word
                          test_sse_parser
                          test_sentence_buffer
                          test_streaming
                          asr_benchmark
                          test_vad_models
                          test_vad_api
                          test_asr_concurrent
                          test_session_commands
                          test_summarizer_quality
                          test_scheduler
                          test_music_db
                          test_memory_embeddings
                          test_memory_filter
                          test_document_extract
                          test_document_chunker
                          test_document_db
                          test_embedding_engine
                          test_plan_executor
                          test_phone_db
                          test_missed_notifications_db
                          test_temporal_relations
                          test_relation_supersede
                          test_time_query_parser
                          test_context_compaction
                          test_word_to_number
                          test_text_filter
                          test_memory_similarity
                          test_rate_limiter
                          test_crypto_store
                          test_input_queue
                          test_config_validate
                          test_calculator
                          test_contacts_db
                          test_calendar_db
                          test_auth_db
                          test_tool_registry
                          test_device_types
                          test_html_parser
                          test_tts_preprocessing
                          test_email_db
                  COMMENT "Building all tests")
