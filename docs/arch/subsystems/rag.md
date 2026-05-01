# Document Search / RAG Subsystem

Source: `src/tools/document_*.c`, `src/core/embedding_engine.c`, `src/webui/webui_doc_library.c`

Part of the [D.A.W.N. architecture](../../../ARCHITECTURE.md) — see the main doc for layer rules, threading model, and lock ordering.

---

**Purpose**: Retrieval-Augmented Generation — upload documents, chunk and embed them, then search or read them via LLM tools.

## Architecture: Shared Embedding Engine + Per-Document Chunking + Dual Tool Interface

```
┌───────────────────────────────────────────────────────────────────────┐
│                     DOCUMENT INGESTION                                 │
├───────────────────────────────────────────────────────────────────────┤
│  WebUI upload (drag-and-drop / file picker)                           │
│  → webui_doc_library.c receives file                                  │
│  → document_chunker.c: paragraph split → sentence split → overlap     │
│  → embedding_engine_embed() per chunk (ONNX/Ollama/OpenAI)           │
│  → document_db.c: store document + chunks + embeddings in SQLite      │
│  → SHA-256 dedup prevents re-indexing identical files                  │
└───────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌───────────────────────────────────────────────────────────────────────┐
│                     LLM TOOL ACCESS                                    │
├───────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  document_search(query)        document_read(document, start, count)  │
│  ├─ Embed query                ├─ Find document by name/ID            │
│  ├─ Load all user chunks       ├─ Load chunks in order (paginated)    │
│  ├─ Cosine similarity rank     ├─ Return text with pagination hint    │
│  ├─ Keyword boost (top 50)     └─ LLM calls again for next page      │
│  └─ Return top 5 with scores                                          │
│                                                                        │
└───────────────────────────────────────────────────────────────────────┘
```

## Key Components

- **embedding_engine.c/h** (`src/core/`): Shared embedding infrastructure
   - Extracted from `memory_embeddings.c` — used by both memory and document systems
   - Multi-provider: ONNX local (`bge-small-en-v1.5-int8`), Ollama, OpenAI-compatible
   - Thread-safe embed() with mutex protection
   - ARM NEON vectorized dot product for cosine similarity
   - `embedding_engine_init()`, `embedding_engine_embed()`, `embedding_engine_cosine()`

- **document_db.c/h** (`src/tools/`): SQLite CRUD for documents and chunks
   - Uses shared `auth.db` handle with prepared statements
   - `document_db_create()`, `document_db_delete()`, `document_db_list()`
   - `document_db_find_by_name()`: fuzzy name lookup with LIKE + exact-match priority
   - `document_db_chunk_create()`, `document_db_chunk_read()`, `document_db_chunk_search_load()`
   - Access control: queries filter by `user_id = ? OR is_global = 1`

- **document_chunker.c/h** (`src/tools/`): Text chunking for embedding
   - Paragraph-aware splitting (double newline boundaries)
   - Sentence boundary splitting for oversized paragraphs
   - Configurable target/max token sizes and overlap

- **document_search.c/h** (`src/tools/`): Semantic search tool
   - Two-pass scoring: cosine similarity first, keyword boost on top 50 only
   - Results formatted with source citations and relevance scores
   - Uses `result_extended` for results exceeding the 8KB fixed buffer

- **document_read.c/h** (`src/tools/`): Paginated document reader
   - Parameters: `document` (name), `start_chunk` (offset), `count` (page size, max 20)
   - Returns ordered chunk text with pagination hint for next page
   - Enables full-document summarization via iterative reading

- **webui_doc_library.c** (`src/webui/`): WebUI Document Library panel
   - Upload with file type validation (PDF, DOCX, TXT, MD)
   - Per-user document count limits (configurable)
   - List, delete, index, and toggle-global WebSocket endpoints
   - Admin "All Users" view with username resolution (JOIN on users table)
   - Global visibility toggle: owner or admin can share/unshare documents
   - Audit logging for admin cross-user operations
   - `conn_check_admin_quiet()` for soft-fallback admin checks (no error response)

- **doc-library.js / doc-library.css** (`www/`): WebUI frontend
   - Drag-and-drop upload with progress indicator
   - Document list with type badges, chunk counts, delete confirmation
   - Admin controls: "All Users" toggle, global upload checkbox, per-document globe toggle
   - Owner badge (username) on documents in admin "All Users" view
   - SVG globe icon with filled/stroke states for global visibility
   - Focus trap and keyboard focus-visible styles for accessibility
   - Mobile-friendly: bottom-sheet layout, 44px+ touch targets

## Database Schema

Two tables in `auth.db`:

```sql
documents (id, user_id, filename, filepath, filetype, file_hash,
           num_chunks, is_global, created_at)

document_chunks (id, document_id, chunk_index, text, embedding, embedding_norm)
```

Indexes: `idx_doc_chunks_doc`, `idx_documents_user`, `idx_documents_hash`.

## Configuration

```toml
[documents]
chunk_target_tokens = 500
chunk_max_tokens = 1000
chunk_overlap_tokens = 50
max_search_results = 5
max_context_tokens = 2000
max_file_size_mb = 10
max_index_size_kb = 2048       # Per-document index size limit
max_indexed_documents = 50     # Per-user document count limit
```
