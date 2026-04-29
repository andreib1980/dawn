# =============================================================================
# Dawn Test Suite
# =============================================================================
# Build all tests:        make tests-all     (or `make tests` — alias kept for
#                                              backwards compatibility)
# Build CI-safe subset:   make tests-ci
# Build a single test:    make test_<name>
#
# Note: This file is included from the main CMakeLists.txt with EXCLUDE_FROM_ALL,
# so tests are not built by default. `enable_testing()` lives in the root
# CMakeLists.txt (BEFORE add_subdirectory) so that ctest discovers tests
# correctly even though the subdirectory is excluded.

set(CMAKE_C_STANDARD 11)

# Unity test framework (vendored, matches STAT/ECHO repos)
# NOTE: DAWN uses EXCLUDE_FROM_ALL for tests because the Jetson target has
# limited resources. This means ctest is a test-runner only — `make tests-ci`
# must be run first to build the binaries.
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

# Source root directory (parent of tests/)
set(SRC_ROOT ${CMAKE_SOURCE_DIR})

# Include directories (inherited from parent, but be explicit for clarity)
include_directories(${SRC_ROOT}/include)

# Aggregate targets — populated by dawn_add_unit_test() below.
add_custom_target(tests-ci  COMMENT "Building CI-safe unit tests")
add_custom_target(tests-all COMMENT "Building all tests")
# Backwards-compatibility alias for `make tests`.
add_custom_target(tests DEPENDS tests-all)

# =============================================================================
# Helper: dawn_add_unit_test
# =============================================================================
# Defines a test executable, registers it with ctest, applies a CTest label
# (ci or hardware), and adds it to the appropriate aggregate target(s).
#
# Single source of truth per test — replaces three previously separate manual
# lists (add_executable / set_tests_properties / aggregate DEPENDS), so a
# forgotten entry can no longer cause CI to silently skip a test.
#
# Usage:
#   dawn_add_unit_test(test_name
#       SOURCES   <source files...>
#       LIBS      <libraries...>           # passed to target_link_libraries
#       [INCLUDES <dirs...>]               # target_include_directories
#       [DEFINES  <defs...>]               # target_compile_definitions
#       [LABEL    ci|hardware]             # CTest label + aggregate membership
#       [NO_TEST])                         # Skip add_test (for benchmarks)
#
# - Tests with `LABEL ci` are added to both `tests-ci` and `tests-all`.
# - Tests with `LABEL hardware` are added to `tests-all` only.
# - Tests with no label are added to `tests-all` (and registered with ctest
#   without a label).
# - NO_TEST skips ctest registration entirely (e.g., bench_retrieval, which is
#   driven by an external Python harness).
function(dawn_add_unit_test NAME)
   cmake_parse_arguments(T "NO_TEST" "LABEL" "SOURCES;LIBS;INCLUDES;DEFINES" ${ARGN})

   add_executable(${NAME} ${T_SOURCES})
   if(T_LIBS)
      target_link_libraries(${NAME} PRIVATE ${T_LIBS})
   endif()
   if(T_INCLUDES)
      target_include_directories(${NAME} PRIVATE ${T_INCLUDES})
   endif()
   if(T_DEFINES)
      target_compile_definitions(${NAME} PRIVATE ${T_DEFINES})
   endif()

   if(NOT T_NO_TEST)
      add_test(NAME ${NAME} COMMAND ${NAME})
      if(T_LABEL STREQUAL "ci")
         set_tests_properties(${NAME} PROPERTIES LABELS "ci")
         add_dependencies(tests-ci ${NAME})
      elseif(T_LABEL STREQUAL "hardware")
         set_tests_properties(${NAME} PROPERTIES LABELS "hardware")
      endif()
   endif()
   add_dependencies(tests-all ${NAME})
endfunction()

# =============================================================================
# Shared source / library groups
# =============================================================================
# Auth-DB chain — three DB-backed tests (calendar, auth, email) link the full
# prepared-statement layer rather than stubbing it.
set(AUTH_DB_SOURCES
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

# CUDA libraries — required by Whisper-backed hardware tests.
set(CUDA_LIBS
    ${CUDA_LIBRARIES}
    ${CUDART_LIBRARIES}
    ${CUSPARSE_LIBRARIES}
    ${CUBLAS_LIBRARIES}
    ${CUSOLVER_LIBRARIES}
    ${CURAND_LIBRARIES})

# =============================================================================
# CI-safe tests (no ONNX, CUDA, hardware, or external API dependencies)
# =============================================================================

dawn_add_unit_test(test_sse_parser
   SOURCES test_sse_parser.c
           ${SRC_ROOT}/src/llm/sse_parser.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_sentence_buffer
   SOURCES test_sentence_buffer.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_session_commands
   SOURCES test_session_commands.c
   LIBS    unity pthread
   LABEL   ci)

dawn_add_unit_test(test_scheduler
   SOURCES test_scheduler.c test_scheduler_stub.c
           ${SRC_ROOT}/src/core/scheduler_db.c
   LIBS    dawn_common unity sqlite3 pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_music_db
   SOURCES test_music_db.c
           ${SRC_ROOT}/src/audio/music_db.c
           ${SRC_ROOT}/src/audio/music_source.c
           ${SRC_ROOT}/src/core/path_utils.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_embeddings
   SOURCES test_memory_embeddings.c test_memory_embeddings_stub.c
           ${SRC_ROOT}/src/memory/memory_embeddings.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/core/embedding_engine.c
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

dawn_add_unit_test(test_context_compaction
   SOURCES test_context_compaction.c test_context_compaction_impl.c
   LIBS    unity ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_time_query_parser
   SOURCES test_time_query_parser.c
           ${SRC_ROOT}/src/core/time_query_parser.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_temporal_relations
   SOURCES test_temporal_relations.c test_temporal_relations_stub.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

dawn_add_unit_test(test_relation_supersede
   SOURCES test_relation_supersede.c test_relation_supersede_stub.c
           ${SRC_ROOT}/src/memory/memory_db.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

# Document extract has conditional MuPDF / libzip+libxml2 linking, so the
# optional libs and defines are appended to the target after creation.
dawn_add_unit_test(test_document_extract
   SOURCES test_document_extract.c
           ${SRC_ROOT}/src/tools/document_extract.c
           ${SRC_ROOT}/src/tools/html_parser.c
   LIBS    dawn_common unity
   INCLUDES ${SRC_ROOT}/include
   LABEL   ci)
if(MUPDF_FOUND)
   target_link_libraries(test_document_extract PRIVATE
       ${MUPDF_LIB} ${MUPDF_THIRD_LIB}
       mujs gumbo openjp2 jbig2dec jpeg
       ${HARFBUZZ_LIB} ${FREETYPE2_LIBRARIES} z m)
   target_compile_definitions(test_document_extract PRIVATE HAVE_MUPDF)
else()
   target_link_libraries(test_document_extract PRIVATE m)
endif()
if(LIBZIP_FOUND AND LIBXML2_FOUND)
   target_link_libraries(test_document_extract PRIVATE
       ${LIBZIP_LIBRARIES} ${LIBXML2_LIBRARIES})
   target_compile_definitions(test_document_extract PRIVATE HAVE_LIBZIP HAVE_LIBXML2)
endif()

dawn_add_unit_test(test_document_chunker
   SOURCES test_document_chunker.c
           ${SRC_ROOT}/src/tools/document_chunker.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_document_db
   SOURCES test_document_db.c test_document_db_stub.c
           ${SRC_ROOT}/src/tools/document_db.c
   LIBS    dawn_common unity sqlite3 pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_embedding_engine
   SOURCES test_embedding_engine.c test_embedding_engine_stub.c
           ${SRC_ROOT}/src/core/embedding_engine.c
   LIBS    dawn_common unity pthread m
   LABEL   ci)

dawn_add_unit_test(test_plan_executor
   SOURCES test_plan_executor.c test_plan_executor_stub.c
           ${SRC_ROOT}/src/tools/plan_executor.c
   LIBS    dawn_common unity ${JSONC_LIBRARIES} pthread
   LABEL   ci)

dawn_add_unit_test(test_wake_word
   SOURCES test_wake_word.c
           ${SRC_ROOT}/src/core/wake_word.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_instruction_loader
   SOURCES test_instruction_loader.c
           ${SRC_ROOT}/src/tools/instruction_loader.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_phone_db
   SOURCES test_phone_db.c test_scheduler_stub.c
           ${SRC_ROOT}/src/tools/phone_db.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_missed_notifications_db
   SOURCES test_missed_notifications_db.c test_scheduler_stub.c
           ${SRC_ROOT}/src/core/missed_notifications_db.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_memory_filter
   SOURCES test_memory_filter.c
           ${SRC_ROOT}/src/memory/memory_filter.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_word_to_number
   SOURCES test_word_to_number.c
           ${SRC_ROOT}/src/word_to_number.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_text_filter
   SOURCES test_text_filter.c
           ${SRC_ROOT}/src/core/text_filter.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_memory_similarity
   SOURCES test_memory_similarity.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   LIBS    unity m
   LABEL   ci)

dawn_add_unit_test(test_rate_limiter
   SOURCES test_rate_limiter.c
           ${SRC_ROOT}/src/core/rate_limiter.c
   LIBS    unity pthread
   LABEL   ci)

dawn_add_unit_test(test_crypto_store
   SOURCES test_crypto_store.c test_crypto_store_stub.c
           ${SRC_ROOT}/src/core/crypto_store.c
   LIBS    dawn_common unity sodium pthread
   LABEL   ci)

dawn_add_unit_test(test_input_queue
   SOURCES test_input_queue.c
           ${SRC_ROOT}/src/input_queue.c
   LIBS    dawn_common unity pthread
   LABEL   ci)

dawn_add_unit_test(test_config_validate
   SOURCES test_config_validate.c
           ${SRC_ROOT}/src/config/config_validate.c
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_calculator
   SOURCES test_calculator.c
           ${SRC_ROOT}/src/tools/calculator.c
           ${SRC_ROOT}/src/tools/tinyexpr.c
   LIBS    dawn_common unity m pthread
   LABEL   ci)

dawn_add_unit_test(test_contacts_db
   SOURCES test_contacts_db.c test_contacts_db_stub.c
           ${SRC_ROOT}/src/memory/contacts_db.c
   LIBS    dawn_common unity sqlite3 pthread
   LABEL   ci)

dawn_add_unit_test(test_calendar_db
   SOURCES test_calendar_db.c
           ${SRC_ROOT}/src/tools/calendar_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

dawn_add_unit_test(test_auth_db
   SOURCES test_auth_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

dawn_add_unit_test(test_tool_registry
   SOURCES test_tool_registry.c test_tool_registry_stub.c
           ${SRC_ROOT}/src/tools/tool_registry.c
   LIBS    dawn_common unity pthread ${JSONC_LIBRARIES}
   LABEL   ci)

dawn_add_unit_test(test_device_types
   SOURCES test_device_types.c
           ${SRC_ROOT}/src/core/device_types.c
   LIBS    dawn_common unity
   LABEL   ci)

dawn_add_unit_test(test_html_parser
   SOURCES test_html_parser.c
           ${SRC_ROOT}/src/tools/html_parser.c
   LIBS    dawn_common unity
   LABEL   ci)

# tts_preprocessing.cpp is compiled directly (rather than linking dawn_common_tts)
# to avoid the Piper / ONNX runtime dependency.
dawn_add_unit_test(test_tts_preprocessing
   SOURCES test_tts_preprocessing.c
           ${SRC_ROOT}/common/src/tts/tts_preprocessing.cpp
   LIBS    unity
   LABEL   ci)

dawn_add_unit_test(test_email_db
   SOURCES test_email_db.c test_email_db_stub.c
           ${SRC_ROOT}/src/tools/email_db.c
           ${AUTH_DB_SOURCES}
   LIBS    dawn_common unity sqlite3 pthread m
   LABEL   ci)

# =============================================================================
# Hardware-dependent tests (require ONNX, CUDA, or external APIs)
# =============================================================================

dawn_add_unit_test(test_streaming
   SOURCES test_streaming.c
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
   LIBS    dawn_common ${CURL_LIBRARIES} ${JSONC_LIBRARIES}
           OpenSSL::SSL OpenSSL::Crypto
   LABEL   hardware)

dawn_add_unit_test(asr_benchmark
   SOURCES asr_benchmark.c
           ${SRC_ROOT}/src/asr/asr_interface.c
           ${SRC_ROOT}/src/asr/asr_vosk.c
   INCLUDES ${SRC_ROOT}/whisper.cpp/include
   LIBS    dawn_common_asr vosk whisper sndfile ${JSONC_LIBRARIES} ${CUDA_LIBS}
   LABEL   hardware)

dawn_add_unit_test(test_vad_models
   SOURCES test_vad_models.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    onnxruntime m
   LABEL   hardware)

dawn_add_unit_test(test_vad_api
   SOURCES test_vad_api.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common_vad onnxruntime m ${JSONC_LIBRARIES}
   LABEL   hardware)

dawn_add_unit_test(test_asr_concurrent
   SOURCES test_asr_concurrent.c
           ${SRC_ROOT}/src/asr/asr_interface.c
           ${SRC_ROOT}/src/ui/metrics.c
   INCLUDES ${SRC_ROOT}/whisper.cpp/include
   LIBS    dawn_common_asr whisper pthread m ${JSONC_LIBRARIES} ${CUDA_LIBS}
   LABEL   hardware)

dawn_add_unit_test(test_summarizer_quality
   SOURCES test_summarizer_quality.c
           ${SRC_ROOT}/src/tools/tfidf_summarizer.c
           ${SRC_ROOT}/src/tools/url_fetcher.c
           ${SRC_ROOT}/src/tools/html_parser.c
           ${SRC_ROOT}/src/config/config_parser.c
           ${SRC_ROOT}/src/config/config_defaults.c
           ${SRC_ROOT}/src/config/config_env.c
           ${SRC_ROOT}/src/tools/toml.c
   LIBS    dawn_common ${CURL_LIBRARIES} ${JSONC_LIBRARIES} m
   LABEL   hardware)

# =============================================================================
# Benchmarks (not run via ctest — driven by external Python harnesses)
# =============================================================================

dawn_add_unit_test(bench_retrieval
   NO_TEST
   SOURCES ${SRC_ROOT}/benchmarks/bench_retrieval.c
           ${SRC_ROOT}/benchmarks/bench_retrieval_stub.c
           ${SRC_ROOT}/src/tools/document_db.c
           ${SRC_ROOT}/src/core/time_query_parser.c
           ${SRC_ROOT}/src/core/embedding_engine.c
           ${SRC_ROOT}/src/memory/memory_embed_onnx.c
           ${SRC_ROOT}/src/memory/memory_embed_ollama.c
           ${SRC_ROOT}/src/memory/memory_embed_openai.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common sqlite3 pthread m onnxruntime
           ${CURL_LIBRARIES} ${JSONC_LIBRARIES})

dawn_add_unit_test(bench_contradiction
   NO_TEST
   SOURCES ${SRC_ROOT}/benchmarks/bench_contradiction.c
           ${SRC_ROOT}/benchmarks/bench_contradiction_stub.c
           ${SRC_ROOT}/src/core/embedding_engine.c
           ${SRC_ROOT}/src/memory/memory_embed_onnx.c
           ${SRC_ROOT}/src/memory/memory_embed_ollama.c
           ${SRC_ROOT}/src/memory/memory_embed_openai.c
           ${SRC_ROOT}/src/memory/memory_similarity.c
   INCLUDES /usr/local/include/onnxruntime
   LIBS    dawn_common pthread m onnxruntime
           ${CURL_LIBRARIES} ${JSONC_LIBRARIES})
