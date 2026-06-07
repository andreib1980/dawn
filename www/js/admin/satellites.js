/**
 * DAWN Satellite Management Module
 * Admin satellite CRUD operations and UI
 */
(function () {
   'use strict';

   let satellites = [];
   let users = [];
   let haAreas = [];
   let refreshInterval = null;
   let callbacks = {
      showConfirmModal: null,
      trapFocus: null,
   };

   const REFRESH_INTERVAL_MS = 30000;
   const LOCAL_PSEUDO_UUID = '00000000-0000-0000-0000-000000000000';

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestListSatellites() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         const list = document.getElementById('satellite-list');
         if (list && satellites.length === 0) {
            list.innerHTML = '<div class="loading-indicator">Loading satellites...</div>';
         }
         DawnWS.send({ type: 'list_satellites' });
      }
   }

   function requestUpdateSatellite(uuid, updates) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'update_satellite',
            payload: { uuid, ...updates },
         });
      }
   }

   function requestDeleteSatellite(uuid) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'delete_satellite',
            payload: { uuid },
         });
      }
   }

   /* =============================================================================
    * Time Helpers
    * ============================================================================= */

   function formatLastSeen(timestamp) {
      if (!timestamp) return 'Never';
      const now = Math.floor(Date.now() / 1000);
      const diff = now - timestamp;
      if (diff < 60) return 'Just now';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return Math.floor(diff / 86400) + 'd ago';
   }

   /* =============================================================================
    * UI Rendering
    * ============================================================================= */

   function buildHaAreaControl(sat) {
      if (haAreas.length > 0) {
         // Dropdown from HA areas
         let options = '<option value="">-- No Area --</option>';
         for (const area of haAreas) {
            const selected = area === (sat.ha_area || '') ? ' selected' : '';
            options += '<option value="' + escapeHtml(area) + '"' + selected + '>';
            options += escapeHtml(area) + '</option>';
         }
         // Add current value if not in list (manually set previously)
         if (sat.ha_area && !haAreas.includes(sat.ha_area)) {
            options +=
               '<option value="' +
               escapeHtml(sat.ha_area) +
               '" selected>' +
               escapeHtml(sat.ha_area) +
               '</option>';
         }
         return (
            '<select class="satellite-ha-area" data-uuid="' +
            sat.uuid +
            '">' +
            options +
            '</select>'
         );
      }
      // Fallback: text input when HA not configured
      return (
         '<input type="text" class="satellite-ha-area" data-uuid="' +
         sat.uuid +
         '" value="' +
         escapeHtml(sat.ha_area || '') +
         '" placeholder="e.g., Living Room">'
      );
   }

   function renderSatelliteList() {
      const list = document.getElementById('satellite-list');
      if (!list) return;

      if (satellites.length === 0) {
         list.innerHTML =
            '<div class="satellite-list-empty">' +
            'No satellites registered. Satellites appear here automatically when they connect.' +
            '</div>';
         return;
      }

      let html = '';
      for (const sat of satellites) {
         const isLocal = sat.uuid === LOCAL_PSEUDO_UUID;
         let tierLabel, tierClass;
         if (isLocal) {
            tierLabel = 'LOCAL';
            tierClass = 'tier-local';
         } else if (sat.tier === 1) {
            tierLabel = 'T1 RPi';
            tierClass = 'tier-1';
         } else {
            tierLabel = 'T2 ESP32';
            tierClass = 'tier-2';
         }
         // Local pseudo-satellite is always "present" — treat as online.
         const effectiveOnline = isLocal ? true : sat.online;
         const statusClass = effectiveOnline ? 'online' : 'offline';
         const statusLabel = effectiveOnline ? 'Online' : 'Offline';
         const lastSeenText =
            isLocal || effectiveOnline ? '' : 'Last seen: ' + formatLastSeen(sat.last_seen);

         // Build user dropdown options
         let userOptions = '<option value="0">-- Unassigned --</option>';
         for (const u of users) {
            const selected = u.id === sat.user_id ? ' selected' : '';
            userOptions +=
               '<option value="' +
               u.id +
               '"' +
               selected +
               '>' +
               escapeHtml(u.display_name) +
               '</option>';
         }

         html +=
            '<div class="satellite-card" data-uuid="' +
            sat.uuid +
            '">' +
            // Header row
            '<div class="satellite-header">' +
            '<span class="satellite-status ' +
            statusClass +
            '" title="' +
            statusLabel +
            '" role="img" aria-label="' +
            statusLabel +
            '"></span>' +
            '<span class="satellite-name">' +
            escapeHtml(sat.name) +
            '</span>' +
            '<span class="satellite-tier ' +
            tierClass +
            '">' +
            tierLabel +
            '</span>' +
            '<span class="satellite-location">' +
            escapeHtml(sat.location || 'No location') +
            '</span>' +
            '<span class="satellite-status-text ' +
            statusClass +
            '">' +
            statusLabel +
            '</span>' +
            '</div>' +
            (lastSeenText ? '<div class="satellite-last-seen">' + lastSeenText + '</div>' : '') +
            // Firmware version + any in-flight OTA state (non-local devices that have reported it)
            (!isLocal && sat.firmware_version
               ? '<div class="satellite-last-seen satellite-firmware">Firmware: ' +
                 escapeHtml(sat.firmware_version) +
                 (sat.ota_state && sat.ota_state !== 'idle'
                    ? ' · ' +
                      escapeHtml(sat.ota_state) +
                      (sat.ota_target_version ? ' → ' + escapeHtml(sat.ota_target_version) : '')
                    : '') +
                 '</div>'
               : '') +
            // Controls row
            '<div class="satellite-controls">' +
            '<label class="satellite-control-group">' +
            '<span class="control-label">User:</span>' +
            '<select class="satellite-user-select" data-uuid="' +
            sat.uuid +
            '">' +
            userOptions +
            '</select>' +
            '</label>' +
            '<label class="satellite-control-group">' +
            '<span class="control-label">HA Area:</span>' +
            buildHaAreaControl(sat) +
            '</label>' +
            '</div>' +
            // Footer: delete button (hidden for the daemon's local pseudo-satellite)
            (isLocal
               ? '<div class="satellite-footer satellite-footer-local">' +
                 '<span class="satellite-local-note">' +
                 'Daemon speaker. <strong>Unassigned</strong> plays for everyone; ' +
                 'assign to a user to restrict notifications to them. Cannot be deleted.' +
                 '</span>' +
                 '</div>'
               : '<div class="satellite-footer">' +
                 '<button class="btn satellite-delete-btn" data-uuid="' +
                 sat.uuid +
                 '" data-name="' +
                 escapeHtml(sat.name) +
                 '" data-user="' +
                 escapeHtml(getUserName(sat.user_id)) +
                 '">Delete Satellite</button>' +
                 '</div>') +
            '</div>';
      }

      list.innerHTML = html;
      attachEventListeners();
   }

   function getUserName(userId) {
      if (!userId) return 'Unassigned';
      const user = users.find((u) => u.id === userId);
      return user ? user.display_name : 'Unknown';
   }

   function escapeHtml(str) {
      return DawnFormat.escapeHtml(str);
   }

   /* =============================================================================
    * Event Listeners
    * ============================================================================= */

   function attachEventListeners() {
      // User assignment dropdown
      document.querySelectorAll('.satellite-user-select').forEach((sel) => {
         sel.addEventListener('change', function () {
            const uuid = this.dataset.uuid;
            const userId = parseInt(this.value, 10);
            this.disabled = true;
            requestUpdateSatellite(uuid, { user_id: userId });
         });
      });

      // HA area (works for both select and input)
      document.querySelectorAll('.satellite-ha-area').forEach((el) => {
         if (el.tagName === 'SELECT') {
            el.addEventListener('change', function () {
               const uuid = this.dataset.uuid;
               this.disabled = true;
               requestUpdateSatellite(uuid, { ha_area: this.value });
            });
         } else {
            el.addEventListener('blur', function () {
               const uuid = this.dataset.uuid;
               const sat = satellites.find((s) => s.uuid === uuid);
               if (sat && this.value !== (sat.ha_area || '')) {
                  this.disabled = true;
                  requestUpdateSatellite(uuid, { ha_area: this.value });
               }
            });
            el.addEventListener('keydown', function (e) {
               if (e.key === 'Enter') this.blur();
            });
         }
      });

      // Delete buttons
      document.querySelectorAll('.satellite-delete-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            const uuid = this.dataset.uuid;
            const name = this.dataset.name;
            const assignedUser = this.dataset.user;
            let msg = "Delete satellite '" + name + "'?";
            if (assignedUser && assignedUser !== 'Unassigned') {
               msg += "\nCurrently assigned to user '" + assignedUser + "'.";
            }
            msg += '\nThis satellite will need to re-register to appear again.';
            if (callbacks.showConfirmModal) {
               callbacks.showConfirmModal(
                  msg,
                  () => {
                     requestDeleteSatellite(uuid);
                  },
                  { title: 'Delete Satellite', okText: 'Delete', danger: true }
               );
            } else if (confirm(msg)) {
               requestDeleteSatellite(uuid);
            }
         });
      });
   }

   /* =============================================================================
    * Auto-Refresh
    * ============================================================================= */

   function startAutoRefresh() {
      stopAutoRefresh();
      refreshInterval = setInterval(requestListSatellites, REFRESH_INTERVAL_MS);
   }

   function stopAutoRefresh() {
      if (refreshInterval) {
         clearInterval(refreshInterval);
         refreshInterval = null;
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (payload.satellites) {
         satellites = payload.satellites;
      }
      if (payload.users) {
         users = payload.users;
      }
      if (payload.ha_areas) {
         haAreas = payload.ha_areas;
      }
      renderSatelliteList();
   }

   function handleUpdateResponse(payload) {
      if (payload.success && payload.satellite) {
         const idx = satellites.findIndex((s) => s.uuid === payload.satellite.uuid);
         if (idx >= 0) {
            satellites[idx] = payload.satellite;
         }
         renderSatelliteList();
      } else {
         renderSatelliteList(); // Re-enable controls
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to update satellite', 'error');
         }
      }
   }

   function handleDeleteResponse(payload) {
      if (payload.success && payload.uuid) {
         satellites = satellites.filter((s) => s.uuid !== payload.uuid);
         renderSatelliteList();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Satellite deleted', 'success');
         }
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to delete satellite', 'error');
         }
      }
   }

   /* =============================================================================
    * Pairing Modal (QR-code surface for Android satellite onboarding)
    * ============================================================================= */

   /* dawn://provision URI schema version.  Increment when the URI parameter
    * shape changes; the Android app rejects unknown versions so a bump forces
    * an explicit acknowledgement on the client side. */
   const PAIR_URI_VERSION = '1';
   const PAIR_IDLE_TIMEOUT_MS = 5 * 60 * 1000; /* auto-close modal after 5 min idle */
   const PAIR_CLIPBOARD_CLEAR_MS = 30 * 1000; /* wipe clipboard 30s after copy */
   const PAIR_RERENDER_DEBOUNCE_MS = 200;
   const PAIR_REVEAL_AUTO_HIDE_MS = 30 * 1000; /* re-mask the key 30s after reveal */

   const pairState = {
      key: '',
      tlsEnabled: false,
      revealed: false,
      lastRenderedUri: '' /* skip identical QR re-renders */,
      focusCleanup: null,
      idleTimer: null,
      clipboardClearTimer: null,
      rerenderTimer: null,
      revealAutoHideTimer: null,
      keydownCleanup: null,
      previousFocus: null,
   };

   /* Cached DOM references — populated once in initPairModal and reused on
    * every modal open.  Avoids 12 getElementById lookups per resetIdleTimer
    * tick (mousemove fires those frequently in long sessions). */
   let pairEls = null;

   function buildPairEls() {
      return {
         modal: document.getElementById('satellite-pair-modal'),
         closeBtn: document.getElementById('satellite-pair-close'),
         qr: document.getElementById('satellite-pair-qr'),
         qrWrap: document.getElementById('satellite-pair-qr-wrap'),
         empty: document.getElementById('satellite-pair-empty'),
         server: document.getElementById('satellite-pair-server'),
         wsWarning: document.getElementById('satellite-pair-ws-warning'),
         keyInput: document.getElementById('satellite-pair-key'),
         keyReveal: document.getElementById('satellite-pair-key-reveal'),
         copyBtn: document.getElementById('satellite-pair-copy'),
         copyStatus: document.getElementById('satellite-pair-copy-status'),
         pairBtn: document.getElementById('pair-satellite-btn'),
      };
   }

   function defaultServerUrl() {
      const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      return scheme + '//' + window.location.host + '/ws';
   }

   function buildPairUri(serverUrl, key) {
      return (
         'dawn://provision?server=' +
         encodeURIComponent(serverUrl) +
         '&v=' +
         PAIR_URI_VERSION +
         '#key=' +
         encodeURIComponent(key)
      );
   }

   function renderQrInto(container, uri) {
      /* qrcode-generator API: type=0 (auto), error-correction 'M' fits ~120-char URIs
       * comfortably. createSvgTag returns an XML-escaped SVG string with black
       * modules on a white background — both required for reliable phone scans
       * against a dark-themed modal.
       *
       * XSS-safe: the URI is bitstream-encoded into Reed-Solomon QR modules and
       * rendered as SVG <rect> elements; input never lands inside an SVG
       * attribute value, so the inputs (server URL + key, both URL-encoded)
       * cannot escape the rendering context. */
      const qr = qrcode(0, 'M');
      qr.addData(uri);
      qr.make();
      container.innerHTML = qr.createSvgTag({ cellSize: 6, margin: 4, scalable: true });
   }

   function scheduleRerender() {
      if (pairState.rerenderTimer) clearTimeout(pairState.rerenderTimer);
      pairState.rerenderTimer = setTimeout(rerenderPairModal, PAIR_RERENDER_DEBOUNCE_MS);
   }

   function setCopyButtonDisabled(disabled, reason) {
      if (!pairEls || !pairEls.copyBtn) return;
      pairEls.copyBtn.disabled = disabled;
      if (disabled && reason) {
         pairEls.copyBtn.setAttribute('aria-describedby', reason);
      } else {
         pairEls.copyBtn.removeAttribute('aria-describedby');
      }
   }

   function rerenderPairModal() {
      if (!pairEls || pairEls.modal.classList.contains('hidden')) return;
      if (!pairState.key) return;
      const serverUrl = (pairEls.server.value || '').trim();
      const isWs = /^ws:\/\//i.test(serverUrl);
      const tlsDowngrade = isWs && pairState.tlsEnabled;
      if (pairEls.wsWarning) {
         pairEls.wsWarning.classList.toggle('hidden', !tlsDowngrade);
      }
      /* When the daemon is serving TLS, refuse to emit a downgraded ws:// payload —
       * the PSK would travel cleartext on the satellite's next register. */
      if (tlsDowngrade) {
         pairEls.qr.innerHTML = '';
         pairState.lastRenderedUri = '';
         setCopyButtonDisabled(true, 'satellite-pair-ws-warning');
         return;
      }
      setCopyButtonDisabled(false);
      const uri = buildPairUri(serverUrl, pairState.key);
      if (uri === pairState.lastRenderedUri) return; /* no change → skip work */
      renderQrInto(pairEls.qr, uri);
      pairState.lastRenderedUri = uri;
   }

   function resetIdleTimer() {
      if (pairState.idleTimer) clearTimeout(pairState.idleTimer);
      pairState.idleTimer = setTimeout(closePairModal, PAIR_IDLE_TIMEOUT_MS);
   }

   function scheduleRevealAutoHide() {
      if (pairState.revealAutoHideTimer) clearTimeout(pairState.revealAutoHideTimer);
      pairState.revealAutoHideTimer = setTimeout(() => {
         if (pairState.revealed) setRevealed(false);
         pairState.revealAutoHideTimer = null;
      }, PAIR_REVEAL_AUTO_HIDE_MS);
   }

   function setRevealed(revealed) {
      pairState.revealed = revealed;
      if (!pairEls || !pairEls.keyInput || !pairEls.keyReveal) return;
      pairEls.keyInput.type = revealed ? 'text' : 'password';
      pairEls.keyReveal.textContent = revealed ? 'Hide' : 'Show';
      pairEls.keyReveal.setAttribute('aria-pressed', revealed ? 'true' : 'false');
      pairEls.keyReveal.setAttribute(
         'aria-label',
         revealed ? 'Hide registration key' : 'Show registration key'
      );
      /* Auto re-mask after 30s so a forgotten-open modal doesn't sit with the
       * key exposed in plain text in the DOM.  Idle auto-close (5min) is a
       * coarser backstop. */
      if (revealed) {
         scheduleRevealAutoHide();
      } else if (pairState.revealAutoHideTimer) {
         clearTimeout(pairState.revealAutoHideTimer);
         pairState.revealAutoHideTimer = null;
      }
   }

   function setCopyStatus(text, isError) {
      if (!pairEls || !pairEls.copyStatus) return;
      pairEls.copyStatus.textContent = text || '';
      pairEls.copyStatus.classList.toggle('satellite-pair-copy-status-error', !!isError);
   }

   function requestRegistrationKey() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'get_satellite_registration_key' });
   }

   /* Tear down every timer the modal owns.  Called from closePairModal and any
    * future caller that needs to abort timers (e.g. WS disconnect mid-modal).
    * Keep this list complete — adding a new timer to pairState without listing
    * it here will leak it on close. */
   function clearPairTimers() {
      const fields = ['idleTimer', 'rerenderTimer', 'clipboardClearTimer', 'revealAutoHideTimer'];
      for (const f of fields) {
         if (pairState[f]) {
            clearTimeout(pairState[f]);
            pairState[f] = null;
         }
      }
   }

   function openPairModal() {
      if (!pairEls || !pairEls.modal) return;
      pairState.previousFocus = document.activeElement;
      pairState.revealed = false;
      pairState.key = '';
      pairState.tlsEnabled = false;
      pairState.lastRenderedUri = '';
      pairEls.keyInput.value = '';
      pairEls.server.value = defaultServerUrl();
      setRevealed(false);
      setCopyStatus('');
      pairEls.qr.innerHTML = '';
      pairEls.empty.classList.add('hidden');
      pairEls.qrWrap.classList.remove('hidden');
      setCopyButtonDisabled(true);
      pairEls.wsWarning.classList.add('hidden');
      pairEls.modal.classList.remove('hidden');
      if (callbacks.trapFocus) {
         pairState.focusCleanup = callbacks.trapFocus(pairEls.modal);
      }
      /* trapFocus puts focus on the first focusable element (the × button).
       * The Copy URI button is the user's likely next action and has clearer
       * semantics when announced first — override after trapFocus runs. */
      if (pairEls.copyBtn) pairEls.copyBtn.focus();
      pairState.keydownCleanup = wireEscapeToClose(pairEls.modal);
      resetIdleTimer();
      requestRegistrationKey();
   }

   function closePairModal() {
      clearPairTimers();
      if (pairState.focusCleanup) {
         pairState.focusCleanup();
         pairState.focusCleanup = null;
      }
      if (pairState.keydownCleanup) {
         pairState.keydownCleanup();
         pairState.keydownCleanup = null;
      }
      /* JS strings are immutable; we can't actually scrub the original heap
       * allocation.  Dropping the reference lets V8 GC the string when it next
       * runs.  Setting to '' here is the most we can do — it does NOT erase
       * the prior in-memory copy. */
      pairState.key = '';
      pairState.tlsEnabled = false;
      pairState.revealed = false;
      pairState.lastRenderedUri = '';
      if (pairEls) {
         if (pairEls.keyInput) pairEls.keyInput.value = '';
         if (pairEls.qr) pairEls.qr.innerHTML = '';
         if (pairEls.modal) pairEls.modal.classList.add('hidden');
      }
      if (pairState.previousFocus && pairState.previousFocus.focus) {
         try {
            pairState.previousFocus.focus();
         } catch (_) {
            /* node removed from DOM — ignore */
         }
      }
      pairState.previousFocus = null;
   }

   function wireEscapeToClose(modal) {
      function handler(e) {
         if (e.key === 'Escape') {
            e.preventDefault();
            closePairModal();
         }
      }
      modal.addEventListener('keydown', handler);
      return () => modal.removeEventListener('keydown', handler);
   }

   function handleRegistrationKeyResponse(payload) {
      if (!pairEls || pairEls.modal.classList.contains('hidden')) return;
      if (!payload) return;
      pairState.tlsEnabled = !!payload.tls_enabled;
      if (!payload.key_set || !payload.key) {
         /* Empty state — no key configured. */
         pairEls.qrWrap.classList.add('hidden');
         pairEls.empty.classList.remove('hidden');
         setCopyButtonDisabled(true);
         return;
      }
      pairState.key = payload.key;
      pairEls.keyInput.value = payload.key;
      rerenderPairModal();
   }

   async function onCopyUri() {
      if (!pairEls || !pairState.key) return;
      const serverUrl = (pairEls.server.value || '').trim();
      const isWs = /^ws:\/\//i.test(serverUrl);
      if (isWs && pairState.tlsEnabled) {
         setCopyStatus('Use wss:// when the server has TLS enabled.', true);
         return;
      }
      const uri = buildPairUri(serverUrl, pairState.key);
      try {
         await DawnFormat.copyToClipboard(uri);
         setCopyStatus('Copied. Local clipboard will clear in 30s.');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Pairing URI copied', 'success');
         }
         if (pairState.clipboardClearTimer) clearTimeout(pairState.clipboardClearTimer);
         pairState.clipboardClearTimer = setTimeout(async () => {
            try {
               await DawnFormat.copyToClipboard('');
               /* Only this device's clipboard is cleared; cross-device sync
                * (Universal Clipboard, KDE Connect, Gboard, etc.) may retain
                * the URI on other devices — there's no browser API to reach
                * those.  The modal disclaimer warns the user explicitly. */
               setCopyStatus('Local clipboard cleared.');
            } catch (_) {
               /* Best-effort — most failures here are benign. */
            }
            pairState.clipboardClearTimer = null;
         }, PAIR_CLIPBOARD_CLEAR_MS);
      } catch (_) {
         setCopyStatus('Copy failed — select the key manually.', true);
      }
   }

   function initPairModal() {
      pairEls = buildPairEls();
      if (!pairEls.modal || !pairEls.pairBtn) {
         pairEls = null;
         return;
      }
      pairEls.pairBtn.addEventListener('click', openPairModal);
      if (pairEls.closeBtn) pairEls.closeBtn.addEventListener('click', closePairModal);
      pairEls.modal.addEventListener('click', function (e) {
         /* Click on backdrop closes; click on modal content does not. */
         if (e.target === pairEls.modal) closePairModal();
      });
      if (pairEls.server) {
         pairEls.server.addEventListener('input', scheduleRerender);
      }
      if (pairEls.keyReveal) {
         pairEls.keyReveal.addEventListener('click', () => setRevealed(!pairState.revealed));
      }
      if (pairEls.copyBtn) {
         pairEls.copyBtn.addEventListener('click', onCopyUri);
      }
      /* Any activity inside the modal pushes the idle auto-close out. */
      ['mousemove', 'mousedown', 'keydown', 'touchstart'].forEach((evt) => {
         pairEls.modal.addEventListener(evt, resetIdleTimer);
      });
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('satellite-management-section');
      if (!section) return;

      // Load on section expand and manage auto-refresh
      // (toggle handled by generic handler in settings.js)
      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', function () {
            // Check after the generic handler has toggled 'collapsed'
            setTimeout(function () {
               if (!section.classList.contains('collapsed')) {
                  if (satellites.length === 0) {
                     requestListSatellites();
                  }
                  startAutoRefresh();
               } else {
                  stopAutoRefresh();
               }
            }, 0);
         });
      }

      // Refresh button
      const refreshBtn = document.getElementById('refresh-satellites-btn');
      if (refreshBtn) {
         refreshBtn.addEventListener('click', requestListSatellites);
      }

      // Pair modal
      initPairModal();
   }

   // Initialize when DOM is ready
   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnSatellites = {
      handleListResponse: handleListResponse,
      handleUpdateResponse: handleUpdateResponse,
      handleDeleteResponse: handleDeleteResponse,
      handleRegistrationKeyResponse: handleRegistrationKeyResponse,
      handleReconnect: function () {
         /* Re-render to un-disable any stuck controls, then refresh if section is open */
         renderSatelliteList();
         const section = document.getElementById('satellite-management-section');
         if (section && !section.classList.contains('collapsed') && satellites.length > 0) {
            requestListSatellites();
         }
      },
      refresh: requestListSatellites,
      stopAutoRefresh: stopAutoRefresh,
      setCallbacks: function (cbs) {
         if (cbs && cbs.showConfirmModal) callbacks.showConfirmModal = cbs.showConfirmModal;
         if (cbs && cbs.trapFocus) callbacks.trapFocus = cbs.trapFocus;
      },
   };
})();
