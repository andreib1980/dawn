/**
 * DAWN Transcript Module
 * Renders chat messages with debug/command separation
 *
 * Usage:
 *   DawnTranscript.addEntry(role, text)        // Add transcript entry (routes debug content)
 *   DawnTranscript.addDebug(label, content)    // Add debug entry directly
 *   DawnTranscript.addNormal(role, text)       // Add normal entry directly
 */
(function (global) {
   'use strict';

   // =============================================================================
   // Thinking Block Helpers
   // =============================================================================

   /**
    * Check if text contains thinking content
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsThinkingContent(text) {
      return text.includes('<dawn:thinking');
   }

   /**
    * Extract thinking content and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{thinking: {content: string, provider: string, duration: string}|null, remaining: string}}
    */
   function extractThinkingContent(text) {
      const thinkingRegex =
         /<dawn:thinking\s+provider="([^"]+)"\s+duration="([^"]+)">\n([\s\S]*?)\n<\/dawn:thinking>\n?/;
      const match = thinkingRegex.exec(text);

      if (!match) {
         return { thinking: null, remaining: text };
      }

      return {
         thinking: {
            provider: match[1],
            duration: match[2],
            content: match[3],
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Create a thinking block element for display
    * @param {Object} thinking - Thinking data {provider, duration, content}
    * @returns {HTMLElement}
    */
   function createThinkingBlock(thinking) {
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI thinking process');

      const providerLabel =
         thinking.provider === 'claude'
            ? 'Claude'
            : thinking.provider === 'local'
              ? 'Local LLM'
              : 'AI';

      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">💭</span>
        <span class="thinking-label">${providerLabel} thought</span>
        <span class="thinking-duration">(${thinking.duration}s)</span>
        <span class="thinking-toggle" aria-hidden="true">▼</span>
      </div>
      <div class="thinking-content">${DawnFormat.escapeHtml(thinking.content).replace(/\n/g, '<br>')}</div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinkingBlock(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinkingBlock(entry);
         }
      });

      return entry;
   }

   /**
    * Toggle thinking block expanded/collapsed state
    * @param {HTMLElement} entry - The thinking block element
    */
   function toggleThinkingBlock(entry) {
      const isCollapsed = entry.classList.contains('collapsed');
      entry.classList.toggle('collapsed', !isCollapsed);

      const header = entry.querySelector('.thinking-header');
      if (header) {
         header.setAttribute('aria-expanded', isCollapsed ? 'true' : 'false');
      }
   }

   // =============================================================================
   // Reasoning Block Helpers (OpenAI o-series)
   // =============================================================================

   /**
    * Check if text contains reasoning token marker
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsReasoningContent(text) {
      return text.includes('<dawn:reasoning');
   }

   /**
    * Extract reasoning tokens and remaining message from text
    * @param {string} text - Text to parse
    * @returns {{reasoning: {tokens: number}|null, remaining: string}}
    */
   function extractReasoningContent(text) {
      const reasoningRegex = /<dawn:reasoning\s+tokens="(\d+)"\/>\n?/;
      const match = reasoningRegex.exec(text);

      if (!match) {
         return { reasoning: null, remaining: text };
      }

      return {
         reasoning: {
            tokens: parseInt(match[1], 10),
         },
         remaining: text.replace(match[0], '').trim(),
      };
   }

   /**
    * Create a reasoning block element for display (OpenAI o-series)
    * @param {Object} reasoning - Reasoning data {tokens}
    * @returns {HTMLElement}
    */
   function createReasoningBlock(reasoning) {
      const entry = document.createElement('div');
      entry.className = 'thinking-block collapsed completed reasoning-only';
      entry.setAttribute('role', 'region');
      entry.setAttribute('aria-label', 'AI reasoning summary');

      const tokens = reasoning.tokens || 0;
      entry.innerHTML = `
      <div class="thinking-header" role="button" tabindex="0" aria-expanded="false">
        <span class="thinking-icon" aria-hidden="true">🧠</span>
        <span class="thinking-label">OpenAI reasoned</span>
        <span class="thinking-duration">(${tokens.toLocaleString()} tokens)</span>
      </div>
      <div class="thinking-content">
        <em>Reasoning content is not accessible from OpenAI reasoning models.</em>
      </div>
    `;

      // Add click handler for toggle
      const header = entry.querySelector('.thinking-header');
      header.addEventListener('click', () => toggleThinkingBlock(entry));
      header.addEventListener('keydown', (e) => {
         if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            toggleThinkingBlock(entry);
         }
      });

      return entry;
   }

   // =============================================================================
   // Image Marker Helpers
   // =============================================================================

   /**
    * Create a secure image element from a validated data URI
    * SECURITY: Only renders images with safe data URI prefixes
    * @param {string} dataUrl - The validated data URI
    * @returns {HTMLElement|null} - Image wrapper element or null if invalid
    */
   function createImageElement(dataUrl) {
      // SECURITY: Validate data URI using DawnVision's security check
      if (!DawnVision || !DawnVision.isSafeDataUri(dataUrl)) {
         console.warn('Transcript: Rejected unsafe image data URI');
         return null;
      }

      const wrapper = document.createElement('div');
      wrapper.className = 'transcript-image-wrapper';

      const img = document.createElement('img');
      img.className = 'transcript-image';
      img.alt = 'Uploaded image';
      img.src = dataUrl;

      // Add click to expand (optional enhancement)
      img.addEventListener('click', () => {
         img.classList.toggle('expanded');
      });

      wrapper.appendChild(img);
      return wrapper;
   }

   // =============================================================================
   // Content Detection Helpers
   // =============================================================================

   /**
    * Check if text contains command tags
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function containsCommandTags(text) {
      return text.includes('<command>') || text.includes('[Tool Result:');
   }

   /**
    * Check if text is ONLY debug content (no user-facing text)
    * @param {string} text - Text to check
    * @returns {boolean}
    */
   function isOnlyDebugContent(text) {
      // Remove all command tags and tool results, see if anything meaningful remains
      const stripped = text
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/\[Tool Result:[\s\S]*?\]/g, '')
         .trim();
      return stripped.length === 0;
   }

   /**
    * Extract non-command text from a mixed message
    * @param {string} text - Text to extract from
    * @returns {string}
    */
   function extractUserFacingText(text) {
      return text
         .replace(/<command>[\s\S]*?<\/command>/g, '')
         .replace(/\[Tool Result:[\s\S]*?\]/g, '')
         .trim();
   }

   /**
    * Extract command/debug portions from a message
    * @param {string} text - Text to extract from
    * @returns {{commands: string[], toolResults: string[]}}
    */
   function extractDebugContent(text) {
      const commands = [];
      const toolResults = [];

      // Extract command tags
      const cmdRegex = /<command>([\s\S]*?)<\/command>/g;
      let match;
      while ((match = cmdRegex.exec(text)) !== null) {
         commands.push(match[0]);
      }

      // Extract tool results
      const toolRegex = /\[Tool Result:[\s\S]*?\]/g;
      while ((match = toolRegex.exec(text)) !== null) {
         toolResults.push(match[0]);
      }

      return { commands, toolResults };
   }

   // =============================================================================
   // Document Chip Rendering
   // =============================================================================

   /** SVG document icon (small, inline) */
   const DOC_ICON_SVG =
      '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
      '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>' +
      '<polyline points="14 2 14 8 20 8"/>' +
      '<line x1="16" y1="13" x2="8" y2="13"/>' +
      '<line x1="16" y1="17" x2="8" y2="17"/>' +
      '<polyline points="10 9 9 9 8 9"/>' +
      '</svg>';

   /**
    * Create a container of document chips for a transcript entry
    * @param {Array<{filename: string, size: string, content: string}>} documents
    * @returns {HTMLElement}
    */
   function createDocumentChips(documents) {
      const container = document.createElement('div');
      container.className = 'transcript-doc-container';

      documents.forEach((doc) => {
         const chip = document.createElement('button');
         chip.className = 'transcript-doc-chip';
         chip.title = `${doc.filename} \u2014 click to view`;
         chip.setAttribute('aria-label', `View document: ${doc.filename}, ${doc.size}`);

         const icon = document.createElement('span');
         icon.className = 'transcript-doc-icon';
         icon.innerHTML = DOC_ICON_SVG;

         const name = document.createElement('span');
         name.className = 'transcript-doc-name';
         name.textContent = doc.filename;

         const size = document.createElement('span');
         size.className = 'transcript-doc-size';
         size.textContent = doc.size;

         chip.appendChild(icon);
         chip.appendChild(name);
         chip.appendChild(size);
         chip.addEventListener('click', () =>
            DawnDocuments.openDocumentViewer(doc.filename, doc.content)
         );
         container.appendChild(chip);
      });

      return container;
   }

   // =============================================================================
   // Entry Formatting
   // =============================================================================

   /**
    * Format command text for debug display
    * @param {string} text - Text to format
    * @returns {string}
    */
   function formatCommandText(text) {
      // Highlight <command> tags in cyan, tool results in green
      return text
         .replace(/(<command>)/g, '<span style="color:#22d3ee">$1</span>')
         .replace(/(<\/command>)/g, '<span style="color:#22d3ee">$1</span>')
         .replace(
            /(\[Tool Result:[\s\S]*?(?:\](?=\s*$)|$))/g,
            '<span style="color:#22c55e">$1</span>'
         );
   }

   // =============================================================================
   // Entry Rendering
   // =============================================================================

   /**
    * Add a debug entry to the transcript
    * @param {string} label - Debug label (command, tool result, etc.)
    * @param {string} content - Debug content
    */
   function addDebugEntry(label, content) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      const entry = document.createElement('div');

      // Determine specific debug class based on label
      let debugClass = 'debug';
      if (label === 'command') {
         debugClass = 'debug command';
      } else if (label === 'tool result') {
         debugClass = 'debug tool-result';
      } else if (label === 'tool call') {
         debugClass = 'debug tool-call';
      }

      entry.className = `transcript-entry ${debugClass}`;
      entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(label)}</div>
      <div class="text">${formatCommandText(DawnFormat.escapeHtml(content))}</div>
    `;
      // Visibility controlled by CSS via body.debug-mode class
      transcript.appendChild(entry);
   }

   /**
    * Add a normal entry to the transcript
    * @param {string} role - Message role (user, assistant, system)
    * @param {string} text - Message text
    */
   async function addNormalEntry(role, text, extractedDocs) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Parse document markers (strip from display, extract for chips)
      // If caller already extracted docs (addTranscriptEntry), use those directly
      let docData;
      if (extractedDocs && extractedDocs.length > 0) {
         docData = { cleanText: text, documents: extractedDocs };
      } else if (DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         docData = DawnDocuments.parseDocumentMarkers(text);
      } else {
         docData = { cleanText: text, documents: [] };
      }

      // Parse image markers if present (user messages may have embedded images)
      let parsed = null;
      let displayText = docData.cleanText;

      if (DawnVision && DawnVision.parseImageMarkers) {
         parsed = DawnVision.parseImageMarkers(displayText);
         displayText = parsed.text;
      }

      // Strip self-inserted <thinking> tags (Claude chain-of-thought leaking into text)
      displayText = displayText.replace(/<thinking>[\s\S]*?<\/thinking>\s*/g, '');

      // Extract <dawn-visual> blocks before markdown/sanitize (DOMPurify strips custom tags)
      let visualBlocks = [];
      // Split text around visual tags iteratively for inline positioning.
      // Produces alternating text/visual segments: text, visual, text, visual, text
      let segments = []; // { type: 'text'|'visual', content: string, visual: object }
      if (typeof DawnVisualRender !== 'undefined') {
         const extracted = DawnVisualRender.extractVisuals(displayText);
         visualBlocks = extracted.visuals;
         if (visualBlocks.length > 0) {
            const tagRe =
               /<dawn-visual\s+title="[^"]*"\s+type="(?:svg|html)">[\s\S]*?<\/dawn-visual>/g;
            let lastIdx = 0;
            let match;
            let vi = 0;
            while ((match = tagRe.exec(displayText)) !== null) {
               const before = displayText.substring(lastIdx, match.index).trim();
               if (before) segments.push({ type: 'text', content: before });
               if (vi < visualBlocks.length) {
                  segments.push({ type: 'visual', visual: visualBlocks[vi++] });
               }
               lastIdx = match.index + match[0].length;
            }
            const after = displayText.substring(lastIdx).trim();
            if (after) segments.push({ type: 'text', content: after });
         } else {
            segments.push({ type: 'text', content: extracted.cleanText });
         }
      } else {
         segments.push({ type: 'text', content: displayText });
      }

      // Create entry with first text segment
      const entry = document.createElement('div');
      entry.className = `transcript-entry ${role}`;
      const firstText =
         segments.length > 0 && segments[0].type === 'text' ? segments[0].content : '';
      entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(role)}</div>
      <div class="text">${DawnFormat.markdown(firstText)}</div>
    `;
      transcript.appendChild(entry);

      // Append remaining segments (visuals + text blocks)
      for (let i = firstText ? 1 : 0; i < segments.length; i++) {
         if (segments[i].type === 'visual') {
            DawnVisualRender.renderVisuals(entry, [segments[i].visual]);
         } else if (segments[i].content) {
            const textDiv = document.createElement('div');
            textDiv.className = 'text';
            textDiv.innerHTML = DawnFormat.markdown(segments[i].content);
            entry.appendChild(textDiv);
         }
      }

      // Scroll immediately after adding entry
      transcript.scrollTop = transcript.scrollHeight;

      // Store raw text and add copy buttons on all .text elements
      const firstTextEl = entry.querySelector('.text');
      entry.querySelectorAll('.text').forEach(function (el) {
         if (!el.getAttribute('data-raw-text')) {
            el.setAttribute('data-raw-text', docData.cleanText);
         }
         DawnFormat.addCopyButtons(el);
         DawnFormat.addMessageCopyButton(el);
      });

      // Render document chips if any documents were attached
      if (docData.documents.length > 0 && firstTextEl) {
         firstTextEl.appendChild(createDocumentChips(docData.documents));
         transcript.scrollTop = transcript.scrollHeight;
      }

      // Load and add images AFTER entry is visible (async, may involve network requests)
      if (parsed && DawnVision.loadParsedImages) {
         const imageDataUrls = await DawnVision.loadParsedImages(parsed);
         if (imageDataUrls.length > 0) {
            const imagesContainer = document.createElement('div');
            imagesContainer.className = 'transcript-images-container';
            for (const dataUrl of imageDataUrls) {
               const imageEl = createImageElement(dataUrl);
               if (imageEl) {
                  imagesContainer.appendChild(imageEl);
               }
            }
            if (imagesContainer.children.length > 0 && firstTextEl) {
               firstTextEl.appendChild(imagesContainer);
               // Scroll again after images are added (they increase height)
               transcript.scrollTop = transcript.scrollHeight;
            }
         }
      }
   }

   /**
    * Prepend a transcript entry (for loading older messages)
    * @param {string} role - Message role
    * @param {string} text - Message text
    * @param {HTMLElement|null} beforeElement - Element to insert before (null = append)
    */
   async function prependTranscriptEntry(role, text, beforeElement) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Handle thinking content in assistant messages
      let thinkingBlock = null;
      if (role === 'assistant' && containsThinkingContent(text)) {
         const { thinking, remaining } = extractThinkingContent(text);
         if (thinking) {
            thinkingBlock = createThinkingBlock(thinking);
            if (remaining.length > 0) {
               text = remaining;
            } else {
               // Only thinking content, insert and return
               if (beforeElement) {
                  transcript.insertBefore(thinkingBlock, beforeElement);
               } else {
                  transcript.appendChild(thinkingBlock);
               }
               return;
            }
         }
      }

      // Handle reasoning content (OpenAI o-series)
      let reasoningBlock = null;
      if (role === 'assistant' && containsReasoningContent(text)) {
         const { reasoning, remaining } = extractReasoningContent(text);
         if (reasoning) {
            reasoningBlock = createReasoningBlock(reasoning);
            if (remaining.length > 0) {
               text = remaining;
            } else {
               if (beforeElement) {
                  transcript.insertBefore(reasoningBlock, beforeElement);
               } else {
                  transcript.appendChild(reasoningBlock);
               }
               return;
            }
         }
      }

      // Skip debug/tool content for prepend (keeping it simple)
      if (role === 'tool' || text.startsWith('[Tool Result:') || text.startsWith('[Tool Call:')) {
         return;
      }

      // Extract user-facing text if mixed content
      if (containsCommandTags(text)) {
         if (isOnlyDebugContent(text)) {
            return; // Skip pure debug content
         }
         text = extractUserFacingText(text);
         if (!text) return;
      }

      // Parse document markers (strip from display, extract for chips)
      let docData = { cleanText: text, documents: [] };
      if (DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         docData = DawnDocuments.parseDocumentMarkers(text);
      }

      // Parse image markers if present (user messages may have embedded images)
      let parsed = null;
      let displayText = docData.cleanText;

      if (DawnVision && DawnVision.parseImageMarkers) {
         parsed = DawnVision.parseImageMarkers(displayText);
         displayText = parsed.text;
      }

      // Split text around visual tags iteratively for inline positioning
      let visualBlocks = [];
      let segments = [];
      if (typeof DawnVisualRender !== 'undefined') {
         const extracted = DawnVisualRender.extractVisuals(displayText);
         visualBlocks = extracted.visuals;
         if (visualBlocks.length > 0) {
            const tagRe =
               /<dawn-visual\s+title="[^"]*"\s+type="(?:svg|html)">[\s\S]*?<\/dawn-visual>/g;
            let lastIdx = 0;
            let match;
            let vi = 0;
            while ((match = tagRe.exec(displayText)) !== null) {
               const before = displayText.substring(lastIdx, match.index).trim();
               if (before) segments.push({ type: 'text', content: before });
               if (vi < visualBlocks.length) {
                  segments.push({ type: 'visual', visual: visualBlocks[vi++] });
               }
               lastIdx = match.index + match[0].length;
            }
            const after = displayText.substring(lastIdx).trim();
            if (after) segments.push({ type: 'text', content: after });
         } else {
            segments.push({ type: 'text', content: extracted.cleanText });
         }
      } else {
         segments.push({ type: 'text', content: displayText });
      }

      // Create the entry with first text segment
      const entry = document.createElement('div');
      entry.className = `transcript-entry ${role}`;
      const firstText =
         segments.length > 0 && segments[0].type === 'text' ? segments[0].content : '';
      entry.innerHTML = `
      <div class="role">${DawnFormat.escapeHtml(role)}</div>
      <div class="text">${DawnFormat.markdown(firstText)}</div>
    `;

      // Append remaining segments (visuals + text blocks)
      for (let i = firstText ? 1 : 0; i < segments.length; i++) {
         if (segments[i].type === 'visual') {
            DawnVisualRender.renderVisuals(entry, [segments[i].visual]);
         } else if (segments[i].content) {
            const textDiv = document.createElement('div');
            textDiv.className = 'text';
            textDiv.innerHTML = DawnFormat.markdown(segments[i].content);
            entry.appendChild(textDiv);
         }
      }

      // Store raw text and add copy buttons on all .text elements
      const firstTextEl = entry.querySelector('.text');
      entry.querySelectorAll('.text').forEach(function (el) {
         if (!el.getAttribute('data-raw-text')) {
            el.setAttribute('data-raw-text', docData.cleanText);
         }
         DawnFormat.addCopyButtons(el);
         DawnFormat.addMessageCopyButton(el);
      });

      // Render document chips if any documents were attached
      if (docData.documents.length > 0 && firstTextEl) {
         firstTextEl.appendChild(createDocumentChips(docData.documents));
      }

      // Insert thinking/reasoning block first if present
      if (thinkingBlock && beforeElement) {
         transcript.insertBefore(thinkingBlock, beforeElement);
      }
      if (reasoningBlock && beforeElement) {
         transcript.insertBefore(reasoningBlock, beforeElement);
      }

      // Insert the entry FIRST (before loading images) for immediate visibility
      if (beforeElement) {
         transcript.insertBefore(entry, beforeElement);
      } else {
         transcript.appendChild(entry);
      }

      // Load and add images AFTER entry is visible (async, may involve network requests)
      if (parsed && DawnVision.loadParsedImages) {
         const imageDataUrls = await DawnVision.loadParsedImages(parsed);
         if (imageDataUrls.length > 0) {
            const imagesContainer = document.createElement('div');
            imagesContainer.className = 'transcript-images-container';
            for (const dataUrl of imageDataUrls) {
               const imageEl = createImageElement(dataUrl);
               if (imageEl) {
                  imagesContainer.appendChild(imageEl);
               }
            }
            if (imagesContainer.children.length > 0 && firstTextEl) {
               firstTextEl.appendChild(imagesContainer);
            }
         }
      }
   }

   /**
    * Add a transcript entry with automatic debug content routing
    * @param {string} role - Message role
    * @param {string} text - Message text
    */
   async function addTranscriptEntry(role, text) {
      const transcript = DawnElements.transcript;
      if (!transcript) return;

      // Remove placeholder if present
      const placeholder = transcript.querySelector('.transcript-placeholder');
      if (placeholder) {
         placeholder.remove();
      }

      // Strip document markers BEFORE any routing logic — document content may
      // contain <command> tags or [Tool Result:] text that would cause misrouting
      let extractedDocs = [];
      if (DawnDocuments && DawnDocuments.parseDocumentMarkers) {
         const docData = DawnDocuments.parseDocumentMarkers(text);
         if (docData.documents.length > 0) {
            extractedDocs = docData.documents;
            text = docData.cleanText;
         }
      }

      // Check for thinking content in assistant messages (from history)
      if (role === 'assistant' && containsThinkingContent(text)) {
         const { thinking, remaining } = extractThinkingContent(text);
         if (thinking) {
            // Add thinking block first
            const thinkingBlock = createThinkingBlock(thinking);
            transcript.appendChild(thinkingBlock);

            // Continue processing the remaining text
            if (remaining.length > 0) {
               text = remaining;
            } else {
               transcript.scrollTop = transcript.scrollHeight;
               return;
            }
         }
      }

      // Check for reasoning content in assistant messages (from history - OpenAI o-series)
      if (role === 'assistant' && containsReasoningContent(text)) {
         const { reasoning, remaining } = extractReasoningContent(text);
         if (reasoning) {
            // Add reasoning block first (before the response)
            const reasoningBlock = createReasoningBlock(reasoning);
            transcript.appendChild(reasoningBlock);

            // Continue processing the remaining text
            if (remaining.length > 0) {
               text = remaining;
            } else {
               transcript.scrollTop = transcript.scrollHeight;
               return;
            }
         }
      }

      // Visual tool results: stash for attachment to the next assistant message.
      // The actual rendering happens in addNormalEntry via extractVisuals() when
      // the assistant response arrives with the visual content appended.
      // (Stashing is handled in dawn.js handleTextMessage via pendingVisualsForSave)
      if (role === 'visual') {
         return;
      }

      // Route tool role messages to debug entries (server sends role:"tool" for tool results)
      if (role === 'tool') {
         addDebugEntry('tool result', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }

      // Detect Claude native tool format: JSON arrays with tool_use or tool_result objects
      // These come from conversation history and look like:
      // [ { "type": "tool_use", "id": "...", "name": "...", "input": {...} } ]
      // [ { "type": "tool_result", "tool_use_id": "...", "content": "..." } ]
      if (text.trimStart().startsWith('[')) {
         try {
            const parsed = JSON.parse(text);
            if (Array.isArray(parsed) && parsed.length > 0) {
               const firstItem = parsed[0];
               if (firstItem.type === 'tool_use') {
                  // Format tool calls nicely
                  parsed.forEach((item) => {
                     const formatted = `[Tool Call: ${item.name}]\n${JSON.stringify(item.input, null, 2)}`;
                     addDebugEntry('tool call', formatted);
                  });
                  transcript.scrollTop = transcript.scrollHeight;
                  return;
               }
               if (firstItem.type === 'tool_result') {
                  // Format tool results nicely
                  parsed.forEach((item) => {
                     const formatted = `[Tool Result: ${item.tool_use_id}]\n${item.content}`;
                     addDebugEntry('tool result', formatted);
                  });
                  transcript.scrollTop = transcript.scrollHeight;
                  return;
               }
            }
         } catch (e) {
            // Not valid JSON, continue with normal processing
         }
      }

      // Special case: Tool calls/results are sent as complete messages
      // These can contain ] characters in the content, so don't try to parse with regex
      if (text.startsWith('[Tool Result:')) {
         addDebugEntry('tool result', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }
      if (text.startsWith('[Tool Call:')) {
         addDebugEntry('tool call', text);
         transcript.scrollTop = transcript.scrollHeight;
         return;
      }

      const hasDebugContent = containsCommandTags(text);

      if (!hasDebugContent) {
         // Pure user-facing message - show normally
         await addNormalEntry(role, text, extractedDocs);
      } else if (isOnlyDebugContent(text)) {
         // Pure debug message (only commands/tool results) - debug only
         // Still render document chips if present
         if (extractedDocs.length > 0) {
            await addNormalEntry(role, '', extractedDocs);
         } else {
            addDebugEntry(`debug (${role})`, text);
         }
      } else {
         // Mixed message - show user-facing text normally AND debug content separately
         const userText = extractUserFacingText(text);
         const { commands, toolResults } = extractDebugContent(text);

         // Add debug entries for commands
         commands.forEach((cmd) => {
            addDebugEntry('command', cmd);
         });

         // Add debug entries for tool results
         toolResults.forEach((result) => {
            addDebugEntry('tool result', result);
         });

         // Add user-facing text if any (or if documents are attached)
         if (userText.length > 0 || extractedDocs.length > 0) {
            await addNormalEntry(role, userText, extractedDocs);
         }
      }

      transcript.scrollTop = transcript.scrollHeight;
   }

   // =============================================================================
   // Export Module
   // =============================================================================

   // =============================================================================
   // Image Lightbox (click-to-enlarge for chat images)
   // =============================================================================

   function setupImageLightbox() {
      var lightbox = document.createElement('div');
      lightbox.className = 'image-lightbox';
      lightbox.setAttribute('role', 'dialog');
      lightbox.setAttribute('aria-modal', 'true');
      lightbox.setAttribute('aria-label', 'Enlarged image');
      lightbox.innerHTML =
         '<button class="image-lightbox-close" aria-label="Close">&times;</button>' +
         '<img alt="Enlarged view">';
      document.body.appendChild(lightbox);

      var lightboxImg = lightbox.querySelector('img');
      var closeBtn = lightbox.querySelector('.image-lightbox-close');
      var previousFocus = null;

      function closeLightbox() {
         lightbox.classList.remove('visible');
         setTimeout(function () {
            lightbox.style.display = 'none';
         }, 200);
         if (previousFocus) {
            previousFocus.focus();
            previousFocus = null;
         }
      }

      /* Delegated click on transcript — catches all images including dynamically added */
      document.addEventListener('click', function (e) {
         var img = e.target.closest('.transcript-entry img');
         if (!img || !img.src) return;

         /* Skip vision upload thumbnails (tiny preview images) */
         if (img.classList.contains('transcript-image')) return;

         previousFocus = document.activeElement;
         lightboxImg.src = img.src;
         lightboxImg.alt = img.alt || 'Enlarged view';
         lightbox.style.display = 'flex';
         /* Trigger reflow for transition */
         lightbox.offsetHeight;
         lightbox.classList.add('visible');
         closeBtn.focus();
      });

      /* Close on backdrop click or close button */
      lightbox.addEventListener('click', function (e) {
         if (e.target === lightbox || e.target === closeBtn) {
            closeLightbox();
         }
      });

      /* Keyboard: Escape to close, Tab trap within lightbox */
      document.addEventListener('keydown', function (e) {
         if (!lightbox.classList.contains('visible')) return;

         if (e.key === 'Escape') {
            closeLightbox();
         } else if (e.key === 'Tab') {
            /* Trap focus within lightbox — only focusable element is close button */
            e.preventDefault();
            closeBtn.focus();
         }
      });
   }

   /* Init lightbox after DOM is ready */
   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', setupImageLightbox);
   } else {
      setupImageLightbox();
   }

   global.DawnTranscript = {
      addEntry: addTranscriptEntry,
      addDebug: addDebugEntry,
      addNormal: addNormalEntry,
      prependEntry: prependTranscriptEntry,
      // Expose helpers for other modules
      containsCommandTags: containsCommandTags,
      isOnlyDebugContent: isOnlyDebugContent,
      extractUserFacingText: extractUserFacingText,
   };
})(window);
