/**
 * DAWN Streaming Module
 * Handles LLM streaming responses (ChatGPT-style real-time text)
 *
 * Usage:
 *   DawnStreaming.handleStart(payload)     // Handle stream_start message
 *   DawnStreaming.handleDelta(payload)     // Handle stream_delta message
 *   DawnStreaming.handleEnd(payload)       // Handle stream_end message
 *   DawnStreaming.finalize()               // Force finalize current stream
 *   DawnStreaming.setCallbacks({ onStateChange, onSaveMessage })
 */
(function (global) {
   'use strict';

   // Callbacks
   let callbacks = {
      onStateChange: null, // (state, detail) => void - notify state changes
      onSaveMessage: null, // (role, text) => void - save message to history
   };

   /**
    * Display label for an LLM provider in the thinking-block header.
    * Used at two sites within this module (handleThinkingStart + finalizeThinking).
    * The reload path lives in transcript.js's IIFE and duplicates this mapping
    * inline — keep the two in sync when adding new providers.
    */
   function providerDisplayLabel(provider) {
      switch (provider) {
         case 'claude':
            return 'Claude';
         case 'local':
            return 'Local LLM';
         case 'openai':
            return 'OpenAI';
         case 'gemini':
            return 'Gemini';
         default:
            return 'AI';
      }
   }

   // =============================================================================
   // Stream Start
   // =============================================================================

   /**
    * Handle stream_start: Create new assistant entry for streaming
    * @param {Object} payload - Stream start payload with stream_id
    */
   function handleStreamStart(payload) {
      console.log('Stream start:', payload);

      // Update status to show responding
      if (callbacks.onStateChange) {
         callbacks.onStateChange('speaking', 'Responding');
      }

      // Cancel any existing stream
      if (DawnState.streamingState.active) {
         console.warn('Stream start received while already streaming');
         finalizeStream();
      }

      // Remove placeholder if present
      const transcript = DawnElements.transcript;
      if (transcript) {
         const placeholder = transcript.querySelector('.transcript-placeholder');
         if (placeholder) {
            placeholder.remove();
         }

         // Create new streaming entry
         const entry = document.createElement('div');
         entry.className = 'transcript-entry assistant streaming';
         entry.setAttribute('aria-live', 'polite'); // Screen readers announce updates
         entry.setAttribute('aria-atomic', 'false'); // Only announce new content
         entry.innerHTML = `
        <div class="role">assistant</div>
        <div class="text"></div>
      `;
         transcript.appendChild(entry);

         // Update streaming state
         DawnState.streamingState.active = true;
         DawnState.streamingState.streamId = payload.stream_id;
         DawnState.streamingState.entryElement = entry;
         DawnState.streamingState.textElement = entry.querySelector('.text');
         DawnState.streamingState.content = '';

         // Reset thinking tokens for new stream (will be populated by reasoning_summary if applicable)
         DawnState.metricsState.last_thinking_tokens = 0;

         transcript.scrollTop = transcript.scrollHeight;
      }

      // Start hesitation ring animation for token timing visualization
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.startHesitation();
      }
   }

   // =============================================================================
   // Stream Delta
   // =============================================================================

   /**
    * Handle stream_delta: Append text to current streaming entry
    * Uses debounced markdown rendering (max 10Hz) to avoid heavyweight parsing per token
    * @param {Object} payload - Stream delta payload with stream_id and delta
    */
   function handleStreamDelta(payload) {
      // Ignore deltas for different stream IDs
      if (
         !DawnState.streamingState.active ||
         payload.stream_id !== DawnState.streamingState.streamId
      ) {
         console.warn(
            'Ignoring stale stream delta:',
            payload.stream_id,
            'expected:',
            DawnState.streamingState.streamId
         );
         return;
      }

      // Track token timing for hesitation ring
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.onTokenEvent();
      }

      // Emit token event for decoupled visualization modules
      if (typeof DawnEvents !== 'undefined') {
         DawnEvents.emit('token', { timestamp: performance.now() });
      }

      // Append delta to content
      DawnState.streamingState.content += payload.delta;

      // Render with markdown formatting for better readability during streaming
      // Debounced to avoid heavy parsing on every token
      const nowMs = performance.now();
      if (
         nowMs - DawnState.streamingState.lastRenderMs >=
         DawnState.streamingState.renderDebounceMs
      ) {
         renderStreamingContent();
         DawnState.streamingState.lastRenderMs = nowMs;
         DawnState.streamingState.pendingRender = false;
      } else if (!DawnState.streamingState.pendingRender) {
         // Schedule a render for when debounce period ends
         DawnState.streamingState.pendingRender = true;
         const delay =
            DawnState.streamingState.renderDebounceMs -
            (nowMs - DawnState.streamingState.lastRenderMs);
         setTimeout(() => {
            if (DawnState.streamingState.active && DawnState.streamingState.pendingRender) {
               renderStreamingContent();
               DawnState.streamingState.lastRenderMs = performance.now();
               DawnState.streamingState.pendingRender = false;
               if (DawnElements.transcript) {
                  DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
               }
            }
         }, delay);
      }

      if (DawnElements.transcript) {
         DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
      }
   }

   /**
    * Render streaming content with markdown formatting
    */
   function renderStreamingContent() {
      if (!DawnState.streamingState.textElement) return;

      // Strip command tags and self-inserted <thinking> tags before rendering.
      // Claude sometimes emits <thinking>...</thinking> in its text even when
      // extended thinking is disabled — these are chain-of-thought, not output.
      const cleanText = DawnState.streamingState.content
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');

      // Apply markdown formatting (visuals are siblings of .text, not children,
      // so innerHTML overwrite does not affect them)
      DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(cleanText);
   }

   // =============================================================================
   // Stream End
   // =============================================================================

   /**
    * Handle stream_end: Finalize streaming entry
    * @param {Object} payload - Stream end payload with stream_id
    */
   function handleStreamEnd(payload) {
      console.log('Stream end:', payload);

      // Ignore end for different stream IDs
      if (payload.stream_id !== DawnState.streamingState.streamId) {
         console.warn(
            'Ignoring stale stream end:',
            payload.stream_id,
            'expected:',
            DawnState.streamingState.streamId
         );
         return;
      }

      finalizeStream();
   }

   /**
    * Finalize the current streaming entry
    */
   function finalizeStream() {
      if (!DawnState.streamingState.active) {
         return;
      }

      // Update status back to idle
      if (callbacks.onStateChange) {
         callbacks.onStateChange('idle', null);
      }

      // Stop hesitation ring animation
      if (typeof DawnVisualization !== 'undefined') {
         DawnVisualization.stopHesitation();
      }

      // Clean up any stale visual progress placeholders (tool failed before completing)
      if (DawnState.streamingState.entryElement) {
         var staleCards =
            DawnState.streamingState.entryElement.querySelectorAll('.dawn-visual-progress');
         staleCards.forEach(function (card) {
            var titleEl = card.querySelector('.dawn-visual-progress-title');
            if (titleEl) titleEl.textContent = 'Visual generation failed';
            if (card._progressTimer) {
               clearInterval(card._progressTimer);
               card._progressTimer = null;
            }
         });
      }

      // Final render to ensure all content is displayed (in case debounce was pending)
      if (DawnState.streamingState.textElement && DawnState.streamingState.content) {
         // Strip self-inserted <thinking> tags from final render
         var finalContent = DawnState.streamingState.content.replace(
            /<thinking>[\s\S]*?<\/thinking>\s*/g,
            ''
         );
         DawnState.streamingState.textElement.innerHTML = DawnFormat.markdown(finalContent);

         // Add copy buttons to code blocks and message copy button
         DawnFormat.addCopyButtons(DawnState.streamingState.textElement);
         DawnState.streamingState.textElement.setAttribute('data-raw-text', finalContent);
         DawnFormat.addMessageCopyButton(DawnState.streamingState.textElement);
      }

      // Remove streaming class
      if (DawnState.streamingState.entryElement) {
         DawnState.streamingState.entryElement.classList.remove('streaming');
      }

      /* Drain pending visuals and interleave with text content for history save.
       * Order: pre-visual text + visual tags + post-visual text.
       * This ensures replay renders visuals inline, not at the end. */
      var visualContent = '';
      if (callbacks.getPendingVisuals) {
         var visuals = callbacks.getPendingVisuals();
         if (visuals && visuals.length > 0) {
            visualContent = '\n' + visuals.join('\n') + '\n';
         }
      }

      var fullContent =
         (DawnState.streamingState.preVisualContent || '') +
         visualContent +
         DawnState.streamingState.content;
      /* Strip self-inserted <thinking> tags from save content */
      fullContent = fullContent.replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');
      if (fullContent && callbacks.onSaveMessage) {
         let contentToSave = fullContent;

         // Persist thinking marker. Two cases:
         //   (a) Finalized thinking content present → save normally with content.
         //   (b) Empty content but provider emitted reasoning tokens (OpenAI Responses
         //       case where the model didn't emit a summary) → still save an empty
         //       thinking marker so the reload path produces ONE merged panel rather
         //       than a standalone "OpenAI reasoned" fallback block.
         const finContent = DawnState.thinkingState.finalizedContent;
         const finProvider = DawnState.thinkingState.finalizedProvider || 'unknown';
         const finDuration = DawnState.thinkingState.finalizedDuration || '0';
         const hasThinkingContent = finContent && finContent.trim();
         const hasReasoningTokens = DawnState.streamingState.reasoningTokens > 0;
         if (hasThinkingContent) {
            contentToSave =
               `<dawn:thinking provider="${finProvider}" duration="${finDuration}">\n` +
               finContent +
               '\n</dawn:thinking>\n' +
               contentToSave;
         } else if (hasReasoningTokens && finProvider !== 'unknown') {
            /* Empty thinking marker — preserves provider/duration for the merged
             * render. Token count attaches via the dawn:reasoning marker below. */
            contentToSave =
               `<dawn:thinking provider="${finProvider}" duration="${finDuration}">\n` +
               '\n</dawn:thinking>\n' +
               contentToSave;
         }

         // Include reasoning tokens if present (OpenAI Responses + o-series)
         if (hasReasoningTokens) {
            contentToSave =
               `<dawn:reasoning tokens="${DawnState.streamingState.reasoningTokens}"/>\n` +
               contentToSave;
         }

         /* Note: visual content is NOT appended here for client-side save.
          * The server appends pending_visual to the assistant message in
          * session_add_message(), so the DB copy already includes it.
          * The client save_message is a backup that may be skipped on
          * server_saved replay. Visual rendering on replay is handled by
          * extractVisuals() in addNormalEntry/prependTranscriptEntry. */

         callbacks.onSaveMessage('assistant', contentToSave);

         // Clear finalized thinking content after saving
         DawnState.thinkingState.finalizedContent = '';
         DawnState.thinkingState.finalizedDuration = '0';
         DawnState.thinkingState.finalizedProvider = null;
      }

      // Reset state
      DawnState.streamingState.active = false;
      DawnState.streamingState.streamId = null;
      DawnState.streamingState.entryElement = null;
      DawnState.streamingState.textElement = null;
      DawnState.streamingState.content = '';
      DawnState.streamingState.preVisualContent = '';
      DawnState.streamingState.pendingRender = false;
      DawnState.streamingState.reasoningTokens = 0;
   }

   /**
    * Set callbacks
    * @param {Object} cbs - Callback functions
    */
   function setCallbacks(cbs) {
      if (cbs.onStateChange) callbacks.onStateChange = cbs.onStateChange;
      if (cbs.onSaveMessage) callbacks.onSaveMessage = cbs.onSaveMessage;
      if (cbs.getPendingVisuals) callbacks.getPendingVisuals = cbs.getPendingVisuals;
   }

   // =============================================================================
   // Thinking Block Handlers (Extended Reasoning)
   // =============================================================================

   /**
    * Handle thinking_start: Create collapsible thinking block
    * @param {Object} payload - { stream_id, provider }
    */
   function handleThinkingStart(payload) {
      console.log('Thinking start:', payload);

      // Cancel any existing thinking block
      if (DawnState.thinkingState.active) {
         console.warn('Thinking start received while already thinking');
         finalizeThinking(true);
      }

      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Remove placeholder if present
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) {
         placeholder.remove();
      }

      // Create thinking block entry
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI thinking process');

      const providerLabel = providerDisplayLabel(payload.provider);

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${providerLabel} is thinking...</span>
        <span class="thinking-duration"></span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="thinking-content" aria-live="polite"></div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinking(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinking(entry);
         }
      });

      // For multi-iteration tool calls (Responses API), the in-flight assistant
      // entry from the prior iteration's stream is in the transcript. Insert the
      // new thinking block before it so the visual order stays
      //   USER → thinking → (tool calls) → thinking → ASSISTANT.
      //
      // Anchor strictly to `.streaming` — querying any `.assistant` matches
      // prior-turn entries too, which would put the first thinking block of a
      // new turn ABOVE the just-typed user message.
      const streamingEntry = transcript.querySelector('.transcript-entry.assistant.streaming');
      if (streamingEntry) {
         transcript.insertBefore(entry, streamingEntry);
      } else {
         /* First thinking_start of a turn — stream hasn't begun yet, just append. */
         transcript.appendChild(entry);
      }

      // Update thinking state
      DawnState.thinkingState.active = true;
      DawnState.thinkingState.streamId = payload.stream_id;
      DawnState.thinkingState.provider = payload.provider;
      DawnState.thinkingState.entryElement = entry;
      DawnState.thinkingState.contentElement = entry.querySelector('.thinking-content');
      DawnState.thinkingState.content = '';
      DawnState.thinkingState.collapsed = true;
      DawnState.thinkingState.startTime = performance.now();

      transcript.scrollTop = transcript.scrollHeight;
   }

   /**
    * Handle thinking_delta: Append text to thinking block
    * @param {Object} payload - { stream_id, delta }
    */
   function handleThinkingDelta(payload) {
      // Ignore deltas for inactive or mismatched streams
      if (!DawnState.thinkingState.active) {
         return;
      }

      // Append delta to content
      DawnState.thinkingState.content += payload.delta;

      // Debounced rendering (same pattern as stream_delta)
      const nowMs = performance.now();
      if (
         nowMs - DawnState.thinkingState.lastRenderMs >=
         DawnState.thinkingState.renderDebounceMs
      ) {
         renderThinkingContent();
         DawnState.thinkingState.lastRenderMs = nowMs;
         DawnState.thinkingState.pendingRender = false;
      } else if (!DawnState.thinkingState.pendingRender) {
         DawnState.thinkingState.pendingRender = true;
         const delay =
            DawnState.thinkingState.renderDebounceMs -
            (nowMs - DawnState.thinkingState.lastRenderMs);
         setTimeout(() => {
            if (DawnState.thinkingState.active && DawnState.thinkingState.pendingRender) {
               renderThinkingContent();
               DawnState.thinkingState.lastRenderMs = performance.now();
               DawnState.thinkingState.pendingRender = false;
            }
         }, delay);
      }
   }

   /**
    * Render thinking content to the element
    */
   function renderThinkingContent() {
      if (!DawnState.thinkingState.contentElement) return;

      // Render as plain text with line breaks during streaming
      const escapedContent = DawnFormat.escapeHtml(DawnState.thinkingState.content);
      DawnState.thinkingState.contentElement.innerHTML = escapedContent.replace(/\n/g, '<br>');

      // Auto-scroll transcript
      if (DawnElements.transcript) {
         DawnElements.transcript.scrollTop = DawnElements.transcript.scrollHeight;
      }
   }

   /**
    * Handle thinking_end: Finalize thinking block
    * @param {Object} payload - { stream_id, has_content }
    */
   function handleThinkingEnd(payload) {
      console.log('Thinking end:', payload);
      finalizeThinking(payload.has_content);
   }

   /**
    * Finalize the current thinking block
    * @param {boolean} hasContent - Whether thinking had content
    */
   function finalizeThinking(hasContent) {
      if (!DawnState.thinkingState.active) {
         return;
      }

      const entry = DawnState.thinkingState.entryElement;

      // Calculate duration for saving
      let durationSec = '0';
      if (DawnState.thinkingState.startTime) {
         const durationMs = performance.now() - DawnState.thinkingState.startTime;
         durationSec = (durationMs / 1000).toFixed(1);
      }

      // Store finalized data for saving with the assistant message
      DawnState.thinkingState.finalizedDuration = durationSec;
      DawnState.thinkingState.finalizedProvider = DawnState.thinkingState.provider || 'unknown';
      DawnState.thinkingState.finalizedContent = DawnState.thinkingState.content;

      if (entry) {
         // Update label and duration
         const label = entry.querySelector('.thinking-label');
         const duration = entry.querySelector('.thinking-duration');

         if (label) {
            const providerLabel = providerDisplayLabel(DawnState.thinkingState.provider);
            // Use "reasoned" instead of "thought" when there's no summary text —
            // OpenAI Responses keeps an empty block alive for the token-count merge.
            const verb = hasContent
               ? 'thought'
               : DawnState.thinkingState.provider === 'openai'
                 ? 'reasoned'
                 : 'thinking';
            label.textContent = `${providerLabel} ${verb}`;
         }

         if (duration) {
            // Token count (if it arrives) is appended later by handleReasoningSummary.
            duration.textContent = formatThinkingStats(durationSec, 0);
         }

         // Add completed class for styling
         entry.classList.add('completed');

         // Final content render
         if (DawnState.thinkingState.contentElement && DawnState.thinkingState.content) {
            const escapedContent = DawnFormat.escapeHtml(DawnState.thinkingState.content);
            DawnState.thinkingState.contentElement.innerHTML = escapedContent.replace(
               /\n/g,
               '<br>'
            );
         }

         // Empty content handling. For OpenAI Responses, reasoning_summary is about
         // to fire with token counts — keep the block so the merge logic can attach
         // tokens, and replace the empty content area with a brief placeholder. For
         // other providers (Claude/local) an empty thinking-block is just noise, so
         // remove it as before.
         if (!hasContent && DawnState.thinkingState.content.trim() === '') {
            if (DawnState.thinkingState.provider === 'openai') {
               if (DawnState.thinkingState.contentElement) {
                  DawnState.thinkingState.contentElement.classList.add('no-summary');
                  DawnState.thinkingState.contentElement.innerHTML =
                     '<em>No reasoning summary emitted for this turn.</em>';
               }
            } else {
               entry.remove();
            }
         }
      }

      // Reset active state but preserve finalized content for saving
      DawnState.thinkingState.active = false;
      DawnState.thinkingState.streamId = null;
      DawnState.thinkingState.entryElement = null;
      DawnState.thinkingState.contentElement = null;
      DawnState.thinkingState.content = '';
      DawnState.thinkingState.pendingRender = false;
      DawnState.thinkingState.startTime = 0;
      // Note: provider, finalizedDuration, finalizedContent preserved for finalizeStream
   }

   /**
    * Toggle thinking block expanded/collapsed state
    * @param {HTMLElement} entry - The thinking block element
    */
   function toggleThinking(entry) {
      const isCollapsed = entry.classList.contains('collapsed');
      entry.classList.toggle('collapsed', !isCollapsed);

      const header = entry.querySelector('.thinking-header');

      if (header) {
         header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
      }
      // Note: Toggle icon rotation is handled by CSS transform, not textContent swap

      // Update state if this is the active thinking block
      if (DawnState.thinkingState.entryElement === entry) {
         DawnState.thinkingState.collapsed = !isCollapsed;
      }
   }

   // =============================================================================
   // Reasoning Summary (OpenAI o-series)
   // =============================================================================

   /**
    * Format a finalized thinking-block duration line with whichever stats are available.
    * @param {string|null} durationSec - "2.0" style duration (no unit), or null
    * @param {number} tokens - reasoning token count, 0 if unknown
    * @returns {string} text like "(2.0s, 43 tokens)" / "(2.0s)" / "(43 tokens)"
    */
   function formatThinkingStats(durationSec, tokens) {
      const parts = [];
      if (durationSec) parts.push(`${durationSec}s`);
      if (tokens > 0) parts.push(`${tokens.toLocaleString()} tokens`);
      return parts.length ? `(${parts.join(', ')})` : '';
   }

   /**
    * Handle reasoning_summary: token count for the most recent reasoning turn.
    *
    * Two cases:
    *   1. A thinking-block was just streamed (e.g. /v1/responses path that surfaces
    *      reasoning_summary_text deltas) — merge the token count into that block's
    *      duration line so the user sees both metrics without two side-by-side panels.
    *   2. No thinking-block exists (e.g. legacy chat-completions o-series, where the
    *      reasoning content is opaque) — fall back to a standalone reasoning-only
    *      block that surfaces the token count alongside an "(no content)" note.
    *
    * @param {Object} payload - { stream_id, reasoning_tokens }
    */
   function handleReasoningSummary(payload) {
      console.log('Reasoning summary:', payload);

      const transcript = DawnElements.transcript;
      if (!transcript) return;

      const tokens = payload.reasoning_tokens || 0;
      if (tokens <= 0) return;

      // Store reasoning tokens for saving with the message regardless of UI path
      DawnState.streamingState.reasoningTokens = tokens;

      // Case 1: merge into the current turn's finalized thinking-block (Responses API).
      // Scope to blocks after the last .entry.user to avoid merging into a prior
      // turn's thinking panel when the provider changes mid-conversation.
      const lastUserEntry = Array.from(transcript.querySelectorAll('.entry.user')).pop();
      const candidates = transcript.querySelectorAll(
         '.thinking-block.completed:not(.reasoning-only)'
      );
      let target = null;
      for (let i = candidates.length - 1; i >= 0; i--) {
         if (
            !lastUserEntry ||
            candidates[i].compareDocumentPosition(lastUserEntry) & Node.DOCUMENT_POSITION_PRECEDING
         ) {
            target = candidates[i];
            break;
         }
      }
      if (target) {
         const durationEl = target.querySelector('.thinking-duration');
         if (durationEl) {
            const match = (durationEl.textContent || '').match(/(\d+(?:\.\d+)?)s/);
            const durationSec = match ? match[1] : null;
            durationEl.textContent = formatThinkingStats(durationSec, tokens);
         }
         return;
      }

      // Case 2: no thinking-block — opaque reasoning, create a standalone summary.
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed reasoning-only';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI reasoning summary');

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">🧠</span>
        <span class="thinking-label">OpenAI reasoned</span>
        <span class="thinking-duration">${formatThinkingStats(null, tokens)}</span>
      </div>
      <div class="thinking-content no-summary">
        <em>No reasoning summary available for this turn.</em>
      </div>
    `;

      // Add click handler for toggle (shows the "not accessible" message)
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinking(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinking(entry);
         }
      });

      // Find the most recent assistant entry and insert before it
      const assistantEntries = transcript.querySelectorAll('.transcript-entry.assistant');
      if (assistantEntries.length > 0) {
         const lastAssistant = assistantEntries[assistantEntries.length - 1];
         transcript.insertBefore(entry, lastAssistant);
      } else {
         // Fallback: append if no assistant entry found
         transcript.appendChild(entry);
      }

      transcript.scrollTop = transcript.scrollHeight;
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   global.DawnStreaming = {
      handleStart: handleStreamStart,
      handleDelta: handleStreamDelta,
      handleEnd: handleStreamEnd,
      finalize: finalizeStream,
      setCallbacks: setCallbacks,
      // Thinking handlers
      handleThinkingStart: handleThinkingStart,
      handleThinkingDelta: handleThinkingDelta,
      handleThinkingEnd: handleThinkingEnd,
      finalizeThinking: finalizeThinking,
      // Reasoning summary (OpenAI o-series)
      handleReasoningSummary: handleReasoningSummary,
   };
})(window);
