# =============================================================================
# Dawn Test Suite
# =============================================================================
# Build all tests: make tests
# Build individual test: make test_<name>
#
# Note: This file is included from the main CMakeLists.txt with EXCLUDE_FROM_ALL,
# so tests are not built by default.

set(CMAKE_C_STANDARD 11)

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
                      ${JSONC_LIBRARIES})

# Test: Sentence Buffer
add_executable(test_sentence_buffer
               test_sentence_buffer.c
)

target_link_libraries(test_sentence_buffer
                      dawn_common
                      ${JSONC_LIBRARIES})

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

# Test: VAD Model Comparison
add_executable(test_vad_models test_vad_models.c)
target_include_directories(test_vad_models PRIVATE /usr/local/include/onnxruntime)
target_link_libraries(test_vad_models onnxruntime m)

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

# Test: Per-Session LLM Commands
# Tests thread-local storage mechanism for command context (session pointer passing)
# This is a self-contained test that validates TLS without needing full infrastructure
add_executable(test_session_commands
               test_session_commands.c)

target_link_libraries(test_session_commands
                      pthread)

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

# Test: Scheduler DB Unit Test
add_executable(test_scheduler
               test_scheduler.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/core/scheduler_db.c)

target_link_libraries(test_scheduler
                      dawn_common
                      sqlite3
                      pthread
                      ${JSONC_LIBRARIES})

# Test: Music DB Unit Test
# Tests search, dedup, browse, source abstraction, and stale deletion
add_executable(test_music_db
               test_music_db.c
               ${SRC_ROOT}/src/audio/music_db.c
               ${SRC_ROOT}/src/audio/music_source.c
               ${SRC_ROOT}/src/core/path_utils.c)

target_link_libraries(test_music_db
                      dawn_common
                      sqlite3
                      pthread)

# Test: Memory Embeddings Math Utilities
# Tests cosine similarity, L2 norm, and hybrid score logic (no ONNX/DB needed)
add_executable(test_memory_embeddings
               test_memory_embeddings.c
               test_memory_embeddings_stub.c
               ${SRC_ROOT}/src/memory/memory_embeddings.c
               ${SRC_ROOT}/src/tools/time_query_parser.c
               ${SRC_ROOT}/src/core/embedding_engine.c)

target_link_libraries(test_memory_embeddings
                      dawn_common
                      sqlite3
                      pthread
                      m)

# Test: Time Query Parser (#3-A)
# Pure module: parses natural-language temporal expressions in queries.
add_executable(test_time_query_parser
               test_time_query_parser.c
               ${SRC_ROOT}/src/tools/time_query_parser.c)

target_link_libraries(test_time_query_parser m)

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
                      sqlite3
                      pthread
                      m)

# Test: Document Extract
# Tests extension validation, Content-Type mapping, plain text extraction, error codes
add_executable(test_document_extract
               test_document_extract.c
               ${SRC_ROOT}/src/tools/document_extract.c
               ${SRC_ROOT}/src/tools/html_parser.c)

target_include_directories(test_document_extract PRIVATE ${SRC_ROOT}/include)
target_link_libraries(test_document_extract dawn_common)

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

# Test: Document Chunker
# Tests paragraph splitting, sentence splitting, overlap, merging, edge cases
add_executable(test_document_chunker
               test_document_chunker.c
               ${SRC_ROOT}/src/tools/document_chunker.c)

target_link_libraries(test_document_chunker m)

# Test: Document DB CRUD
# Tests create, get, list, delete, global toggle, find_by_name, chunk ops
add_executable(test_document_db
               test_document_db.c
               test_document_db_stub.c
               ${SRC_ROOT}/src/tools/document_db.c)

target_link_libraries(test_document_db
                      dawn_common
                      sqlite3
                      pthread
                      ${JSONC_LIBRARIES})

# Test: Embedding Engine Math
# Tests L2 norm, cosine similarity (no model/provider needed)
add_executable(test_embedding_engine
               test_embedding_engine.c
               test_embedding_engine_stub.c
               ${SRC_ROOT}/src/core/embedding_engine.c)

target_link_libraries(test_embedding_engine
                      dawn_common
                      pthread
                      m)

# Test: Plan Executor
# Tests variable store, conditions, interpolation, parsing, execution, safety limits
add_executable(test_plan_executor
               test_plan_executor.c
               test_plan_executor_stub.c
               ${SRC_ROOT}/src/tools/plan_executor.c)

target_link_libraries(test_plan_executor
                      dawn_common
                      ${JSONC_LIBRARIES}
                      pthread)

# Test: Wake Word Matching
# Tests normalization, wake word detection, command extraction, goodbye/cancel/ignore
add_executable(test_wake_word
               test_wake_word.c
               ${SRC_ROOT}/src/core/wake_word.c)

target_link_libraries(test_wake_word
                      dawn_common)

# Test: Instruction Loader
# Tests file loading, path traversal sanitization, module concatenation, edge cases
add_executable(test_instruction_loader
               test_instruction_loader.c
               ${SRC_ROOT}/src/tools/instruction_loader.c)

target_link_libraries(test_instruction_loader
                      dawn_common)

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
               ${SRC_ROOT}/src/tools/time_query_parser.c
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

# Test: Phone DB Unit Test
add_executable(test_phone_db
               test_phone_db.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/tools/phone_db.c)

target_link_libraries(test_phone_db
                      dawn_common
                      sqlite3
                      pthread)

# Test: Missed Notifications DB Unit Test
add_executable(test_missed_notifications_db
               test_missed_notifications_db.c
               test_scheduler_stub.c
               ${SRC_ROOT}/src/core/missed_notifications_db.c)

target_link_libraries(test_missed_notifications_db
                      dawn_common
                      sqlite3
                      pthread)

# Test: Memory Injection Filter
# Pure module: blocked pattern detection, normalization, ReAct co-occurrence.
add_executable(test_memory_filter
               test_memory_filter.c
               ${SRC_ROOT}/src/memory/memory_filter.c)

target_link_libraries(test_memory_filter
                      dawn_common)

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
                  COMMENT "Building all tests")
