/*
 * DAWN Watches Panel (SAGE proactive attention)
 *
 * Lives as a tab inside the Scheduler & Watches popover (scheduler-queue.js
 * owns the tab strip + shows/hides this pane).  Lists the user's watches with
 * their live current value, lets them toggle / edit threshold / delete, and add
 * a new watch from the metric catalog.  The conversational `attention` LLM tool
 * is the other surface onto the same per-user attention_rules rows.
 *
 * Network surface (WebUI WS request/response):
 *   - Outbound: { type: "watch_list" }
 *   - Outbound: { type: "watch_add",         payload: { metric, direction?, threshold?, notify? } }
 *   - Outbound: { type: "watch_update",      payload: { id, direction?, threshold?, notify? } }
 *   - Outbound: { type: "watch_set_enabled", payload: { id, enabled } }
 *   - Outbound: { type: "watch_remove",      payload: { id } }
 *   - Inbound:  watch_list_response { payload: { watches:[...], catalog:[...], attention_enabled } }
 *   - Inbound:  watch_{add,update,set_enabled,remove}_response { payload: { success, error } }
 *
 * Security: every string from the WS payload that lands in the DOM goes through
 * escapeHtml.  The server scopes all CRUD to the session user_id — the client
 * never sends a user id.
 */
(function () {
   'use strict';

   /* =========================================================================
    * State
    * ========================================================================= */

   const state = {
      watches: [],
      catalog: [],
      attentionEnabled: true,
      active: false /* true while the Watches tab is the visible pane */,
      pollTimer: null,
   };

   /* Live-value refresh cadence while the pane is open (ms).  Matches the
    * scheduler-queue countdown ticker's "only churn while visible" model. */
   const POLL_INTERVAL_MS = 5000;

   /* =========================================================================
    * Helpers
    * ========================================================================= */

   function isConnected() {
      return typeof DawnWS !== 'undefined' && DawnWS.isConnected && DawnWS.isConnected();
   }

   function showToast(msg, type) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(msg, type || 'info');
      }
   }

   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str || '');
      }
      if (str == null) return '';
      return String(str)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;');
   }

   /* Compact numeric formatting for a live reading. */
   function fmtNum(v) {
      if (typeof v !== 'number' || !isFinite(v)) return '';
      if (Number.isInteger(v)) return String(v);
      if (Math.abs(v) >= 100) return String(Math.round(v));
      return String(parseFloat(v.toFixed(1)));
   }

   function withUnit(v, unit) {
      const n = fmtNum(v);
      if (n === '') return '';
      return unit ? n + ' ' + unit : n;
   }

   /* One-line "what this watch does" summary. */
   function conditionText(w) {
      if (w.rule_type === 'absence') {
         return 'Alerts if the feed goes silent for over ' + (w.absence_after_sec || 0) + 's';
      }
      const thr = typeof w.threshold === 'number' ? withUnit(w.threshold, w.unit) : '—';
      const dir =
         w.direction === 'below' ? 'below' : w.direction === 'rising' ? 'rising past' : 'above';
      return 'Alerts when ' + dir + ' ' + thr;
   }

   function currentText(w) {
      if (!w.has_current || typeof w.current !== 'number') return '';
      if (w.rule_type === 'absence') {
         return 'silent ' + fmtNum(w.current) + 's';
      }
      return 'now ' + withUnit(w.current, w.unit);
   }

   /* =========================================================================
    * Network
    * ========================================================================= */

   function requestList() {
      if (!isConnected()) return;
      DawnWS.send({ type: 'watch_list' });
   }

   function requestAdd(data) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'watch_add', payload: data });
   }

   function requestUpdate(data) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'watch_update', payload: data });
   }

   function requestSetEnabled(id, enabled) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'watch_set_enabled', payload: { id: id, enabled: enabled } });
   }

   function requestRemove(id) {
      if (!isConnected()) return;
      DawnWS.send({ type: 'watch_remove', payload: { id: id } });
   }

   /* =========================================================================
    * Response handlers
    * ========================================================================= */

   function handleListResponse(payload) {
      if (!payload || !payload.success) {
         showToast('Failed to load watches', 'error');
         return;
      }
      state.watches = Array.isArray(payload.watches) ? payload.watches : [];
      state.catalog = Array.isArray(payload.catalog) ? payload.catalog : [];
      state.attentionEnabled = payload.attention_enabled !== false;
      render();
   }

   function handleMutationResponse(payload, okMsg) {
      if (payload && payload.success) {
         if (okMsg) showToast(okMsg, 'success');
         requestList();
      } else {
         showToast((payload && payload.error) || 'Action failed', 'error');
      }
   }

   function handleAddResponse(payload) {
      hideModal();
      handleMutationResponse(payload, 'Watch saved');
   }

   function handleUpdateResponse(payload) {
      hideModal();
      handleMutationResponse(payload, 'Watch updated');
   }

   function handleSetEnabledResponse(payload) {
      /* Toggle feedback is the re-render; keep it quiet. */
      handleMutationResponse(payload, null);
   }

   function handleRemoveResponse(payload) {
      handleMutationResponse(payload, 'Watch removed');
   }

   /* =========================================================================
    * Rendering
    * ========================================================================= */

   function render() {
      const list = document.getElementById('watches-list');
      if (!list) return;

      let html = '';

      if (!state.attentionEnabled) {
         html +=
            '<div class="watches-off-note" role="note">' +
            '<span class="watches-off-dot" aria-hidden="true"></span>' +
            'Proactive attention is currently <strong>off</strong>. Watches are saved but ' +
            "won't fire until you enable it in Settings → Proactive Attention." +
            '</div>';
      }

      if (state.watches.length === 0) {
         html +=
            '<div class="watches-empty">' +
            '<p class="watches-empty-headline">Not watching anything yet</p>' +
            '<p class="watches-empty-hint">Add a watch below, or just say ' +
            '<code>"watch the CO2"</code> or <code>"tell me if the battery gets low"</code>.</p>' +
            '</div>';
         list.innerHTML = html;
         return;
      }

      for (const w of state.watches) {
         const disabledClass = w.enabled ? '' : ' is-paused';
         const loud = w.notify === 'alert';
         const badgeClass = loud ? 'accent' : 'muted';
         /* notify is always resolved to a concrete mode server-side; 'digest' is
          * the P0 log-only placeholder (no voice, no banner) — label it honestly
          * rather than the misleading "Digest". */
         const badgeText = loud ? 'Spoken' : w.notify === 'ambient' ? 'Silent' : 'Log only';
         const cur = currentText(w);
         const nameEsc = escapeHtml(w.label || w.metric);

         html += '<div class="watch-card' + disabledClass + '" data-id="' + w.id + '">';
         html += '  <div class="watch-card-header">';
         html += '    <div class="watch-card-title">';
         html += '      <span class="watch-name">' + nameEsc + '</span>';
         html += '      <span class="dawn-badge ' + badgeClass + '">' + badgeText + '</span>';
         if (!w.enabled) html += '<span class="dawn-badge warning">Paused</span>';
         html += '    </div>';
         html += '    <div class="watch-card-actions">';
         html +=
            '      <button class="btn btn-secondary btn-small" data-action="edit" data-id="' +
            w.id +
            '">Edit</button>';
         html +=
            '      <button class="btn-danger-small" data-action="remove" data-id="' +
            w.id +
            '">Remove</button>';
         html += '    </div>';
         html += '  </div>';
         html += '  <div class="watch-card-condition">' + escapeHtml(conditionText(w));
         if (cur) html += ' <span class="watch-card-current">· ' + escapeHtml(cur) + '</span>';
         html += '  </div>';
         /* A real <label> so the "Enabled" text is clickable + associated; the
          * per-watch aria-label keeps SR output distinguishable across cards. */
         html += '  <label class="dawn-toggle-row watch-card-access">';
         html +=
            '    <input type="checkbox" class="dawn-toggle" data-action="toggle-enabled" data-id="' +
            w.id +
            '"' +
            (w.enabled ? ' checked' : '') +
            ' aria-label="Enable watch: ' +
            nameEsc +
            '">';
         html += '    <span class="dawn-toggle-label">Enabled</span>';
         html += '  </label>';
         html += '</div>';
      }

      list.innerHTML = html;
   }

   /* =========================================================================
    * Event delegation
    * ========================================================================= */

   async function handleListClick(e) {
      const btn = e.target.closest('[data-action]');
      if (!btn) return;
      const action = btn.dataset.action;
      const id = parseInt(btn.dataset.id, 10);
      if (action === 'edit') {
         const w = state.watches.find((x) => x.id === id);
         if (w) showEditModal(w);
      } else if (action === 'remove') {
         const w = state.watches.find((x) => x.id === id);
         const name = w ? w.label || w.metric : 'this watch';
         const msg = 'Stop watching ' + name + '?';
         if (
            await DawnDialog.confirm(msg, { title: 'Remove Watch', okText: 'Remove', danger: true })
         ) {
            requestRemove(id);
         }
      }
   }

   function handleListChange(e) {
      const toggle = e.target.closest('[data-action="toggle-enabled"]');
      if (toggle) {
         const id = parseInt(toggle.dataset.id, 10);
         requestSetEnabled(id, toggle.checked);
      }
   }

   /* =========================================================================
    * Add / Edit modal
    * ========================================================================= */

   let modalEl = null;
   let previousFocusEl = null;
   let escToken = null; // DawnEscStack registration while the modal is shown

   // Reveal the modal and register its Escape-dismiss on the global stack (the
   // focus trap below stays element-scoped on modalEl).
   function revealModal() {
      modalEl.classList.remove('hidden');
      if (escToken === null) {
         escToken = DawnEscStack.register(() => {
            hideModal();
            return true;
         });
      }
   }

   function handleModalKeydown(e) {
      if (e.key === 'Tab' && modalEl) {
         const focusable = Array.from(
            modalEl.querySelectorAll(
               'button, input, select, textarea, [tabindex]:not([tabindex="-1"])'
            )
         ).filter((el) => el.offsetParent !== null);
         if (focusable.length === 0) return;
         const first = focusable[0];
         const last = focusable[focusable.length - 1];
         if (e.shiftKey && document.activeElement === first) {
            e.preventDefault();
            last.focus();
         } else if (!e.shiftKey && document.activeElement === last) {
            e.preventDefault();
            first.focus();
         }
      }
   }

   function createModal() {
      if (modalEl) return modalEl;
      modalEl = document.createElement('div');
      modalEl.className = 'modal hidden';
      modalEl.setAttribute('role', 'dialog');
      modalEl.setAttribute('aria-modal', 'true');
      modalEl.setAttribute('aria-labelledby', 'watch-modal-title');
      modalEl.innerHTML =
         '<div class="modal-content watch-modal-content" id="watch-modal-content"></div>';
      modalEl.addEventListener('click', (e) => {
         if (e.target === modalEl) hideModal();
      });
      modalEl.addEventListener('keydown', handleModalKeydown);
      document.body.appendChild(modalEl);
      return modalEl;
   }

   function hideModal() {
      if (modalEl) modalEl.classList.add('hidden');
      if (escToken !== null) {
         DawnEscStack.unregister(escToken);
         escToken = null;
      }
      if (previousFocusEl) {
         previousFocusEl.focus();
         previousFocusEl = null;
      }
   }

   /* Shared field markup — direction / threshold / level. `w` is the current
    * watch when editing (prefill), or null when adding. */
   function conditionFields(w) {
      const unit = w ? w.unit : '';
      const dir = w ? w.direction : '';
      const thr = w && typeof w.threshold === 'number' ? w.threshold : '';
      const level = w ? w.notify : '';
      const isAbsence = w && w.rule_type === 'absence';

      let html = '';
      if (!isAbsence) {
         html += '<div class="form-group"><label for="watch-direction">Trigger direction</label>';
         html += '  <select id="watch-direction" name="direction">';
         html +=
            '    <option value=""' + (!dir ? ' selected' : '') + '>Default for metric</option>';
         html +=
            '    <option value="above"' +
            (dir === 'above' ? ' selected' : '') +
            '>Goes above</option>';
         html +=
            '    <option value="below"' +
            (dir === 'below' ? ' selected' : '') +
            '>Goes below</option>';
         html += '  </select>';
         html += '</div>';
      }

      const thrLabel = isAbsence ? 'Silence window (seconds)' : 'Threshold';
      const thrHint = isAbsence
         ? 'How many seconds of silence before alerting.'
         : 'Leave blank to use the sensible default for this metric.' +
           (unit ? ' Unit: ' + unit : '');
      html +=
         '<div class="form-group"><label for="watch-threshold">' +
         escapeHtml(thrLabel) +
         '</label>';
      html +=
         '  <input type="number" id="watch-threshold" name="threshold" step="any" value="' +
         (thr === '' ? '' : escapeHtml(String(thr))) +
         '" placeholder="' +
         (isAbsence ? '60' : 'Default') +
         '">';
      html += '  <span class="form-hint">' + escapeHtml(thrHint) + '</span>';
      html += '</div>';

      html += '<div class="form-group"><label for="watch-level">Loudness</label>';
      html += '  <select id="watch-level" name="level">';
      html += '    <option value=""' + (!level ? ' selected' : '') + '>Default for metric</option>';
      html +=
         '    <option value="alert"' +
         (level === 'alert' ? ' selected' : '') +
         '>Spoken alert + banner</option>';
      html +=
         '    <option value="ambient"' +
         (level === 'ambient' ? ' selected' : '') +
         '>Silent banner only</option>';
      html += '  </select>';
      html += '</div>';
      return html;
   }

   function showAddModal() {
      if (state.catalog.length === 0) {
         showToast('No watchable metrics available yet', 'info');
         return;
      }
      /* Only offer metrics not already watched — the one-watch-per-metric model
       * means "Add" on an existing metric would silently reset it to defaults, so
       * steer the user to Edit instead (the metric is already in the list). */
      const watched = new Set(state.watches.map((w) => w.metric));
      const available = state.catalog.filter((c) => !watched.has(c.key));
      if (available.length === 0) {
         showToast(
            'You already have a watch for every available signal — edit one instead',
            'info'
         );
         return;
      }
      previousFocusEl = document.activeElement;
      createModal();
      const content = document.getElementById('watch-modal-content');

      let html = '<h3 id="watch-modal-title">Add Watch</h3>';
      html += '<div class="dawn-form watch-add-form">';
      html += '<div class="form-group"><label for="watch-metric">Signal</label>';
      html += '  <select id="watch-metric" name="metric">';
      for (const c of available) {
         const unit = c.unit ? ' (' + c.unit + ')' : '';
         html +=
            '    <option value="' +
            escapeHtml(c.key) +
            '">' +
            escapeHtml((c.label || c.key) + unit) +
            '</option>';
      }
      html += '  </select>';
      html += '</div>';
      html += conditionFields(null);
      html += '<div class="form-error" id="watch-form-error" role="alert"></div>';
      html += '<div class="form-actions">';
      html += '  <button class="btn btn-secondary" id="watch-cancel-btn">Cancel</button>';
      html += '  <button class="btn btn-primary" id="watch-save-btn">Add Watch</button>';
      html += '</div>';
      html += '</div>';
      content.innerHTML = html;

      revealModal();
      document.getElementById('watch-cancel-btn').addEventListener('click', hideModal);
      document.getElementById('watch-save-btn').addEventListener('click', submitAdd);
      const first = modalEl.querySelector('select, input, button');
      if (first) first.focus();
   }

   function showEditModal(w) {
      previousFocusEl = document.activeElement;
      createModal();
      const content = document.getElementById('watch-modal-content');

      let html = '<h3 id="watch-modal-title">Edit Watch</h3>';
      html += '<div class="dawn-form watch-add-form" data-id="' + w.id + '">';
      html +=
         '<div class="form-group"><label>Signal</label>' +
         '<div class="watch-edit-metric">' +
         escapeHtml(w.label || w.metric) +
         '</div></div>';
      html += conditionFields(w);
      html += '<div class="form-error" id="watch-form-error" role="alert"></div>';
      html += '<div class="form-actions">';
      html += '  <button class="btn btn-secondary" id="watch-cancel-btn">Cancel</button>';
      html += '  <button class="btn btn-primary" id="watch-update-btn">Update</button>';
      html += '</div>';
      html += '</div>';
      content.innerHTML = html;

      revealModal();
      document.getElementById('watch-cancel-btn').addEventListener('click', hideModal);
      document
         .getElementById('watch-update-btn')
         .addEventListener('click', () => submitUpdate(w.id));
      const first = modalEl.querySelector('select, input, button');
      if (first) first.focus();
   }

   /* Collect direction / threshold / level from the modal into a payload,
    * omitting any field left at "default" so the server keeps the metric's
    * catalog default (mirrors the tool's optional-override semantics). */
   function collectConditionFields(into) {
      const dirEl = document.getElementById('watch-direction');
      if (dirEl && dirEl.value) into.direction = dirEl.value;
      const levelEl = document.getElementById('watch-level');
      if (levelEl && levelEl.value) into.notify = levelEl.value;
      const thrEl = document.getElementById('watch-threshold');
      if (thrEl && thrEl.value !== '') {
         const t = parseFloat(thrEl.value);
         if (!isNaN(t) && isFinite(t)) into.threshold = t;
      }
   }

   function submitAdd() {
      const metricEl = document.getElementById('watch-metric');
      if (!metricEl || !metricEl.value) {
         const err = document.getElementById('watch-form-error');
         if (err) {
            err.textContent = 'Pick a signal to watch.';
            err.classList.add('visible');
         }
         return;
      }
      const data = { metric: metricEl.value };
      collectConditionFields(data);
      requestAdd(data);
   }

   function submitUpdate(id) {
      const data = { id: id };
      collectConditionFields(data);
      requestUpdate(data);
   }

   /* =========================================================================
    * Pane lifecycle — driven by scheduler-queue when the Watches tab shows
    * ========================================================================= */

   function startPoll() {
      stopPoll();
      state.pollTimer = setInterval(() => {
         if (state.active && isConnected()) requestList();
      }, POLL_INTERVAL_MS);
   }

   function stopPoll() {
      if (state.pollTimer) {
         clearInterval(state.pollTimer);
         state.pollTimer = null;
      }
   }

   function activate() {
      state.active = true;
      requestList();
      startPoll();
   }

   function deactivate() {
      state.active = false;
      stopPoll();
      hideModal();
   }

   /* =========================================================================
    * Init
    * ========================================================================= */

   function init() {
      const list = document.getElementById('watches-list');
      if (list) {
         list.addEventListener('click', handleListClick);
         list.addEventListener('change', handleListChange);
      }
      const addBtn = document.getElementById('watches-add-btn');
      if (addBtn) addBtn.addEventListener('click', showAddModal);
   }

   /* =========================================================================
    * Export
    * ========================================================================= */

   window.DawnWatches = {
      init: init,
      activate: activate,
      deactivate: deactivate,
      handleListResponse: handleListResponse,
      handleAddResponse: handleAddResponse,
      handleUpdateResponse: handleUpdateResponse,
      handleSetEnabledResponse: handleSetEnabledResponse,
      handleRemoveResponse: handleRemoveResponse,
   };
})();
