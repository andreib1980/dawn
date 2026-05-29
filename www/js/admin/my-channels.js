/**
 * DAWN Messaging Channels Module
 *
 * User-scoped management of the current user's linked messaging channels
 * (list / generate link code / unlink / rename / re-enable).  Mirrors the
 * satellite admin panel's state-array + DawnWS request + response-handler +
 * re-render shape, but is USER-scoped (the backend filters by the
 * authenticated user), so it lives in the always-visible settings area
 * rather than an admin-only section.  Operations key on the stable row id;
 * display_name is editable in place.  See docs/MESSAGING_CHANNELS_DESIGN.md
 * §13 Phase 6.
 */
(function () {
   'use strict';

   let channels = [];
   let refreshInterval = null;
   let codeCountdownTimer = null;
   const callbacks = { showConfirmModal: null };

   const REFRESH_INTERVAL_MS = 30000;

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function wsReady() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected();
   }

   function requestList() {
      if (!wsReady()) return;
      const list = document.getElementById('channel-list');
      if (list && channels.length === 0) {
         list.innerHTML = '<div class="loading-indicator">Loading channels...</div>';
      }
      DawnWS.send({ type: 'list_channels' });
   }

   function requestCreateCode(provider) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'create_link_code', payload: provider ? { provider } : {} });
   }

   function requestUnlink(id) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'unlink_channel', payload: { id } });
   }

   function requestRename(id, name) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'rename_channel', payload: { id, name } });
   }

   function requestReenable(id) {
      if (!wsReady()) return;
      DawnWS.send({ type: 'reenable_channel', payload: { id } });
   }

   /* =============================================================================
    * Helpers
    * ============================================================================= */

   function escapeHtml(str) {
      return DawnFormat.escapeHtml(str);
   }

   // Quote-safe escaper for HTML attribute values (escapeHtml does NOT
   // escape quotes — using it inside value="…"/data-…="…" is an attribute
   // breakout / stored-XSS vector since display_name is user-controlled).
   function escapeAttr(str) {
      return DawnFormat.escapeAttr(str);
   }

   function formatLastUsed(timestamp) {
      if (!timestamp) return 'Never';
      const now = Math.floor(Date.now() / 1000);
      const diff = now - timestamp;
      if (diff < 60) return 'Just now';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return Math.floor(diff / 86400) + 'd ago';
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function renderList() {
      const list = document.getElementById('channel-list');
      if (!list) return;

      if (channels.length === 0) {
         list.innerHTML =
            '<div class="channel-list-empty">' +
            'No channels linked yet. Use "Link a channel" below to connect ' +
            'Telegram, Slack, Discord, or SMS.' +
            '</div>';
         return;
      }

      let html = '';
      for (const ch of channels) {
         const enabled = ch.enabled !== false;
         const dotClass = enabled ? 'success' : '';
         const textClass = enabled ? 'online' : 'offline';
         const statusLabel = enabled ? 'Active' : 'Unlinked';
         html +=
            '<div class="channel-card' +
            (enabled ? '' : ' channel-disabled') +
            '" data-id="' +
            ch.id +
            '">' +
            '<div class="channel-header">' +
            '<span class="dawn-status-dot ' +
            dotClass +
            '" title="' +
            statusLabel +
            '" role="img" aria-label="' +
            statusLabel +
            '"></span>' +
            (enabled
               ? '<input type="text" class="channel-name" data-id="' +
                 ch.id +
                 '" value="' +
                 escapeAttr(ch.name) +
                 '" title="Click to rename" aria-label="Channel name (editable)">'
               : '<span class="channel-name-static">' + escapeHtml(ch.name) + '</span>') +
            '<span class="dawn-badge">' +
            escapeHtml(ch.provider) +
            '</span>' +
            '<span class="channel-status-text ' +
            textClass +
            '">' +
            statusLabel +
            '</span>' +
            '</div>' +
            '<div class="channel-meta">Last active: ' +
            formatLastUsed(ch.last_used_at) +
            '</div>' +
            '<div class="channel-controls">' +
            (enabled
               ? '<button class="btn btn-secondary channel-unlink-btn" data-id="' +
                 ch.id +
                 '" data-name="' +
                 escapeAttr(ch.name) +
                 '">Unlink</button>'
               : '<button class="btn btn-primary channel-reenable-btn" data-id="' +
                 ch.id +
                 '">Re-enable</button>') +
            '</div>' +
            '</div>';
      }

      list.innerHTML = html;
      attachListeners();
   }

   function attachListeners() {
      // Inline rename: edit the name field, blur or Enter to commit.
      document.querySelectorAll('.channel-name').forEach((el) => {
         el.addEventListener('blur', function () {
            const id = parseInt(this.dataset.id, 10);
            const ch = channels.find((c) => c.id === id);
            const newName = this.value.trim();
            if (ch && newName && newName !== ch.name) {
               requestRename(id, newName);
            } else if (ch && !newName) {
               this.value = ch.name; // reject empty rename
            }
         });
         el.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') this.blur();
         });
      });

      document.querySelectorAll('.channel-unlink-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            const id = parseInt(this.dataset.id, 10);
            const name = this.dataset.name;
            const msg =
               'Unlink channel "' +
               name +
               '"?\nIt will stop reaching the assistant. You can re-enable it ' +
               'later — the conversation history is preserved.';
            if (callbacks.showConfirmModal) {
               callbacks.showConfirmModal(msg, () => requestUnlink(id), {
                  title: 'Unlink Channel',
                  okText: 'Unlink',
                  danger: true,
               });
            } else if (confirm(msg)) {
               requestUnlink(id);
            }
         });
      });

      document.querySelectorAll('.channel-reenable-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            requestReenable(parseInt(this.dataset.id, 10));
         });
      });
   }

   /* =============================================================================
    * Link-code inline panel
    * ============================================================================= */

   function showLinkCode(payload) {
      const box = document.getElementById('channel-link-code');
      if (!box) return;
      const code = payload.code || '';
      const provider = payload.provider || '';
      let ttl = payload.ttl_seconds || 0;
      const sendForm = (provider === 'slack' ? 'link ' : '/link ') + code;
      const instr = provider
         ? 'Send <code>' +
           escapeHtml(sendForm) +
           '</code> ' +
           (provider === 'sms'
              ? 'as a text to the DAWN number'
              : 'to the bot on ' + escapeHtml(provider)) +
           '.'
         : 'Send <code>/link ' +
           escapeHtml(code) +
           '</code> from the chat client (use "link ' +
           escapeHtml(code) +
           '" on Slack).';

      box.innerHTML =
         '<div class="channel-code-row">' +
         '<input class="channel-code-display" type="text" readonly value="' +
         escapeAttr(code) +
         '" aria-label="Link code">' +
         '<button class="btn btn-secondary channel-code-copy">Copy</button>' +
         '<span class="channel-code-ttl"></span>' +
         '</div>' +
         '<div class="channel-code-instr">' +
         instr +
         '</div>';
      box.classList.remove('hidden');

      const ttlEl = box.querySelector('.channel-code-ttl');
      const copyBtn = box.querySelector('.channel-code-copy');
      if (copyBtn) {
         copyBtn.addEventListener('click', function () {
            DawnFormat.copyToClipboard(code)
               .then(function () {
                  if (typeof DawnToast !== 'undefined')
                     DawnToast.show('Link code copied', 'success');
               })
               .catch(function () {
                  /* clipboard denied — the code is still selectable in the field */
               });
         });
      }

      if (codeCountdownTimer) clearInterval(codeCountdownTimer);
      function tick() {
         if (!ttlEl) return;
         if (ttl <= 0) {
            ttlEl.textContent = 'Expired — generate a new code.';
            clearInterval(codeCountdownTimer);
            codeCountdownTimer = null;
            return;
         }
         const m = Math.floor(ttl / 60);
         const s = ttl % 60;
         ttlEl.textContent = 'Expires in ' + m + ':' + (s < 10 ? '0' : '') + s;
         ttl--;
      }
      tick();
      codeCountdownTimer = setInterval(tick, 1000);
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (payload && Array.isArray(payload.channels)) {
         channels = payload.channels;
      }
      renderList();
   }

   function handleCreateCodeResponse(payload) {
      if (payload) showLinkCode(payload);
   }

   /* Unlink / rename / re-enable success → re-fetch the authoritative list.
    * On failure the daemon sends a generic error toast and the list is left
    * unchanged, so there is no stuck UI to reset. */
   function handleMutationResponse() {
      requestList();
   }

   /* =============================================================================
    * Auto-Refresh
    * ============================================================================= */

   function startAutoRefresh() {
      stopAutoRefresh();
      refreshInterval = setInterval(requestList, REFRESH_INTERVAL_MS);
   }

   function stopAutoRefresh() {
      if (refreshInterval) {
         clearInterval(refreshInterval);
         refreshInterval = null;
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('messaging-channels-section');
      if (!section) return;

      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', function () {
            // Run after the generic settings toggle flips 'collapsed'.
            setTimeout(function () {
               if (!section.classList.contains('collapsed')) {
                  if (channels.length === 0) requestList();
                  startAutoRefresh();
               } else {
                  stopAutoRefresh();
               }
            }, 0);
         });
      }

      const refreshBtn = document.getElementById('refresh-channels-btn');
      if (refreshBtn) refreshBtn.addEventListener('click', requestList);

      const genBtn = document.getElementById('channel-generate-code-btn');
      const provSel = document.getElementById('channel-provider-select');
      if (genBtn) {
         genBtn.addEventListener('click', function () {
            requestCreateCode(provSel ? provSel.value : '');
         });
      }
   }

   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnMessaging = {
      handleListResponse: handleListResponse,
      handleCreateCodeResponse: handleCreateCodeResponse,
      handleMutationResponse: handleMutationResponse,
      handleReconnect: function () {
         stopAutoRefresh();
         renderList();
         const section = document.getElementById('messaging-channels-section');
         if (section && !section.classList.contains('collapsed')) {
            requestList();
            startAutoRefresh();
         }
      },
      refresh: requestList,
      stopAutoRefresh: stopAutoRefresh,
      setCallbacks: function (cbs) {
         if (cbs && cbs.showConfirmModal) callbacks.showConfirmModal = cbs.showConfirmModal;
      },
   };
})();
