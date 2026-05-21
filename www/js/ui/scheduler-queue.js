/**
 * DAWN Scheduler Queue Panel
 *
 * Surfaces the user's scheduled-events queue (timers / alarms / reminders /
 * tasks / briefings) with a per-row delete affordance.  Companion module to
 * scheduler.js (which owns the fire-time banners).
 *
 * Network surface:
 *   - Outbound: { type: "scheduler_action", payload: { action: "list" } }
 *   - Outbound: { type: "scheduler_action", payload: { action: "cancel" | "cancel_occurrence", event_id } }
 *   - Inbound:  scheduler_events_response { payload: { events: [...] } }
 *   - Inbound:  scheduler_events_changed { } (refetch trigger)
 *
 * Security: every string from the WS payload that ends up in the DOM goes
 * through DawnFormat.escapeHtml (or .textContent on a freshly-created node).
 * Fields covered: name, message, source_location, recurrence_days,
 * original_time, tool_name, tool_action, tool_value.
 */
(function () {
   'use strict';

   /* =============================================================================
    * State
    * ============================================================================= */

   const state = {
      events: [],
      activeTab: 'all',
      searchQuery: '',
      isOpen: false,
      armedRowId: null /* event_id of the row whose delete is in "Confirm?" state */,
      armedTimer: null,
      clearMissedArmed: false,
      clearMissedTimer: null,
      countdownTimer: null,
      lastRingingIds: new Set(),
      docClickHandler: null,
      menuOpenForId: null,
      triggerEl: null,
      /* event_ids of rows the user has expanded to show briefing steps.
       * Lost on broadcast refresh — acceptable since the steps don't change
       * (a briefing's step list is set at create time and cloned on
       * recurrence; they don't drift between fires). */
      expandedRowIds: new Set(),
   };

   const els = {
      btn: null,
      popover: null,
      closeBtn: null,
      list: null,
      searchInput: null,
      tabs: null,
      statActive: null,
      statSnoozed: null,
      statRinging: null,
      statMissed: null,
      clearMissedBtn: null,
      srAnnounce: null,
   };

   /* =============================================================================
    * Type metadata
    * ============================================================================= */

   const TYPE_LABEL = {
      timer: 'Timer',
      alarm: 'Alarm',
      reminder: 'Reminder',
      task: 'Task',
      briefing: 'Briefing',
   };

   /* Inline SVG glyphs per event type — 16x16, currentColor stroke. */
   const TYPE_ICON_SVG = {
      timer:
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<path d="M10 2h4"/><path d="M12 14V8"/>' +
         '<circle cx="12" cy="14" r="8"/></svg>',
      alarm:
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/>' +
         '<path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"/></svg>',
      reminder:
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<rect x="4" y="4" width="16" height="16" rx="2"/>' +
         '<path d="M8 10h8M8 14h5"/></svg>',
      task:
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg>',
      briefing:
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>' +
         '<polyline points="14 2 14 8 20 8"/>' +
         '<line x1="8" y1="13" x2="16" y2="13"/>' +
         '<line x1="8" y1="17" x2="14" y2="17"/></svg>',
   };

   const RECURRENCE_LABEL = {
      once: '',
      daily: 'daily',
      weekdays: 'weekdays',
      weekends: 'weekends',
      weekly: 'weekly',
      custom: 'custom',
   };

   /* =============================================================================
    * Time formatting helpers
    * ============================================================================= */

   function pad2(n) {
      return n < 10 ? '0' + n : '' + n;
   }

   function formatCountdown(secs) {
      if (secs < 0) secs = 0;
      if (secs < 3600) {
         const m = Math.floor(secs / 60);
         const s = secs % 60;
         return m + ':' + pad2(s);
      }
      const h = Math.floor(secs / 3600);
      const rem = secs % 3600;
      return h + ':' + pad2(Math.floor(rem / 60)) + ':' + pad2(rem % 60);
   }

   function sameDay(a, b) {
      return (
         a.getFullYear() === b.getFullYear() &&
         a.getMonth() === b.getMonth() &&
         a.getDate() === b.getDate()
      );
   }

   function isTomorrow(target, now) {
      const t = new Date(now);
      t.setDate(t.getDate() + 1);
      return sameDay(target, t);
   }

   function formatAbsTime(ts) {
      const d = new Date(ts * 1000);
      let h = d.getHours();
      const m = d.getMinutes();
      const ampm = h >= 12 ? 'PM' : 'AM';
      h = h % 12;
      if (h === 0) h = 12;
      return h + ':' + pad2(m) + ' ' + ampm;
   }

   /* Human-readable "when": "in 4 minutes", "tomorrow at 7:00 AM", etc. */
   function formatWhen(event) {
      const now = new Date();
      const fireTs = event.effective_fire_at || event.fire_at;
      const fire = new Date(fireTs * 1000);
      const diffSec = Math.floor((fire - now) / 1000);

      if (event.status === 'missed') {
         const day = sameDay(fire, now)
            ? 'today'
            : isTomorrow(fire, new Date(now.getTime() - 86400000))
              ? 'yesterday'
              : fire.toLocaleDateString(undefined, {
                   weekday: 'short',
                   month: 'short',
                   day: 'numeric',
                });
         return 'missed ' + day + ' ' + formatAbsTime(fireTs);
      }
      if (event.status === 'ringing') return 'RINGING NOW';
      if (event.status === 'snoozed') {
         return 'snoozed until ' + formatAbsTime(fireTs);
      }

      if (diffSec <= 60) return 'in ' + Math.max(0, diffSec) + 's';
      if (diffSec < 3600) return 'in ' + Math.floor(diffSec / 60) + ' minutes';
      if (sameDay(fire, now)) return 'in ' + Math.floor(diffSec / 3600) + ' hours';
      if (isTomorrow(fire, now)) return 'tomorrow at ' + formatAbsTime(fireTs);
      if (diffSec < 86400 * 7) {
         const wd = fire.toLocaleDateString(undefined, { weekday: 'short' });
         return wd + ' at ' + formatAbsTime(fireTs);
      }
      const monthDay = fire.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
      return monthDay + ' at ' + formatAbsTime(fireTs);
   }

   /* Right-column time string — countdown for short-fuse timers, absolute otherwise. */
   function formatTimeColumn(event) {
      const now = Math.floor(Date.now() / 1000);
      const fireTs = event.effective_fire_at || event.fire_at;

      if (event.status === 'missed') return formatAbsTime(fireTs);
      if (event.status === 'ringing') return formatAbsTime(fireTs);

      const diff = fireTs - now;
      if (event.event_type === 'timer' && diff > 0 && diff < 3600) {
         return formatCountdown(diff);
      }
      return formatAbsTime(fireTs);
   }

   /* =============================================================================
    * DOM helpers
    * ============================================================================= */

   function escape(s) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(s || '');
      }
      const div = document.createElement('div');
      div.textContent = s == null ? '' : String(s);
      return div.innerHTML;
   }

   /* escape() above is for text-context insertion only — it does NOT escape
    * quotes, so it's UNSAFE for `attr="..."` interpolation.  Use escapeAttr
    * for every HTML attribute value that includes user/LLM-supplied data. */
   function escapeAttr(s) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeAttr) {
         return DawnFormat.escapeAttr(s);
      }
      if (s == null) return '';
      return String(s)
         .replace(/&/g, '&amp;')
         .replace(/"/g, '&quot;')
         .replace(/'/g, '&#39;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;');
   }

   function recurrenceMeta(event) {
      const r = RECURRENCE_LABEL[event.recurrence] || '';
      if (!r) return '';
      if (event.recurrence === 'custom' && event.recurrence_days) {
         return r + ' (' + escape(event.recurrence_days) + ')';
      }
      return r;
   }

   function buildMetaLine(event) {
      const bits = [];
      bits.push('<span class="sched-row-when">' + escape(formatWhen(event)) + '</span>');
      if (event.source_location) {
         bits.push('<span class="sched-row-meta-sep" aria-hidden="true">·</span>');
         bits.push('<span class="sched-row-source">' + escape(event.source_location) + '</span>');
      }
      const rec = recurrenceMeta(event);
      if (rec) {
         bits.push('<span class="sched-row-meta-sep" aria-hidden="true">·</span>');
         bits.push('<span class="sched-row-recurrence">' + escape(rec) + '</span>');
      }
      if (event.status === 'snoozed' && event.snooze_count > 0) {
         bits.push('<span class="sched-row-meta-sep" aria-hidden="true">·</span>');
         bits.push('<span class="sched-row-snooze-count">×' + event.snooze_count + '</span>');
      }
      return bits.join('');
   }

   function buildToolLine(event) {
      if (event.event_type !== 'task' && event.event_type !== 'briefing') return '';
      /* Multi-step briefing: surface "runs N steps" with a click target.  The
       * expanded view (rendered separately by buildStepsPanel) shows each
       * step on its own line. */
      if (event.event_type === 'briefing' && Array.isArray(event.steps) && event.steps.length > 0) {
         const isExpanded = state.expandedRowIds.has(event.id);
         const chevron = isExpanded ? '▼' : '▶';
         const n = event.steps.length;
         return (
            '<div class="sched-row-tool sched-row-tool-expandable" data-expand-trigger="1">' +
            '<span class="sched-row-tool-chevron" aria-hidden="true">' +
            chevron +
            '</span> runs ' +
            n +
            ' step' +
            (n === 1 ? '' : 's') +
            '</div>'
         );
      }
      /* Single-tool (legacy single-step briefing or task) — same inline shape
       * the panel shipped with originally. */
      if (!event.tool_name) return '';
      const action = event.tool_action || '';
      const value = event.tool_value || '';
      const verb = event.event_type === 'briefing' ? 'briefs' : 'runs';
      const txt =
         verb +
         ': ' +
         event.tool_name +
         (action ? '.' + action : '') +
         (value ? ' (' + value + ')' : '');
      return '<div class="sched-row-tool">' + escape(txt) + '</div>';
   }

   /* Expanded steps panel — rendered inside the row body when the row is in
    * state.expandedRowIds.  One <ol> with one <li> per step. */
   function buildStepsPanel(event) {
      if (event.event_type !== 'briefing') return '';
      if (!Array.isArray(event.steps) || event.steps.length === 0) return '';
      if (!state.expandedRowIds.has(event.id)) return '';
      let html = '<ol class="sched-row-steps">';
      for (let i = 0; i < event.steps.length; i++) {
         const s = event.steps[i];
         const name = s.tool_name || '';
         const action = s.tool_action || '';
         const value = s.tool_value || '';
         const tail = action ? '.' + action : '';
         const argText = value ? ' (' + value + ')' : '';
         html +=
            '<li><span class="sched-row-steps-tool">' +
            escape(name + tail) +
            '</span>' +
            (argText ? '<span class="sched-row-steps-arg">' + escape(argText) + '</span>' : '') +
            '</li>';
      }
      html += '</ol>';
      return html;
   }

   function buildRowHTML(event) {
      const type = event.event_type || 'timer';
      const icon = TYPE_ICON_SVG[type] || '';
      const stateClasses = [];
      if (event.status === 'snoozed') stateClasses.push('is-snoozed');
      if (event.status === 'ringing') stateClasses.push('is-ringing');
      if (event.status === 'missed') stateClasses.push('is-missed');
      const armed = state.armedRowId === event.id ? ' is-armed' : '';

      let deleteLabel;
      let deleteAria;
      switch (event.status) {
         case 'ringing':
            deleteLabel = 'Dismiss';
            deleteAria = 'Dismiss ringing ' + (event.name || TYPE_LABEL[type] || '');
            break;
         case 'missed':
            deleteLabel = 'Acknowledge';
            deleteAria = 'Acknowledge missed ' + (event.name || '');
            break;
         default:
            deleteLabel = 'Cancel';
            deleteAria = 'Cancel ' + (event.name || '');
            break;
      }

      const snoozeGlyph =
         event.status === 'snoozed'
            ? '<span class="sched-row-snooze-glyph" aria-hidden="true">Z</span>'
            : '';

      return (
         '<li class="sched-row sched-type-' +
         type +
         ' ' +
         stateClasses.join(' ') +
         armed +
         '" data-event-id="' +
         event.id +
         '"' +
         ' data-type="' +
         escapeAttr(type) +
         '" data-status="' +
         escapeAttr(event.status) +
         '"' +
         ' data-recurrence="' +
         escapeAttr(event.recurrence) +
         '"' +
         ' tabindex="0">' +
         '<div class="sched-row-accent" aria-hidden="true"></div>' +
         '<div class="sched-row-icon" aria-hidden="true">' +
         icon +
         '</div>' +
         '<span class="sched-row-badge">' +
         escape(TYPE_LABEL[type] || type) +
         '</span>' +
         '<div class="sched-row-body">' +
         '<div class="sched-row-name">' +
         snoozeGlyph +
         escape(event.name || '') +
         '</div>' +
         '<div class="sched-row-meta">' +
         buildMetaLine(event) +
         '</div>' +
         buildToolLine(event) +
         buildStepsPanel(event) +
         '</div>' +
         '<time class="sched-row-time">' +
         escape(formatTimeColumn(event)) +
         '</time>' +
         '<div class="sched-row-actions">' +
         '<button type="button" class="sched-row-delete"' +
         ' aria-label="' +
         escapeAttr(deleteAria) +
         '"' +
         ' data-delete-label="' +
         escapeAttr(deleteLabel) +
         '">' +
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"' +
         ' stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
         '<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2m-9 0v14a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2V6"/>' +
         '</svg>' +
         '</button>' +
         '</div>' +
         '</li>'
      );
   }

   /* =============================================================================
    * Filtering + sorting
    * ============================================================================= */

   function isActiveStatus(s) {
      return s === 'pending' || s === 'snoozed' || s === 'ringing';
   }

   function eventMatchesTab(event, tab) {
      if (tab === 'all') return isActiveStatus(event.status);
      if (tab === 'missed') return event.status === 'missed';
      return isActiveStatus(event.status) && event.event_type === tab;
   }

   function eventMatchesSearch(event, q) {
      if (!q) return true;
      const qq = q.toLowerCase();
      const fields = [event.name, event.message, event.source_location];
      return fields.some((f) => f && f.toLowerCase().indexOf(qq) >= 0);
   }

   function sortForTab(events, tab) {
      if (tab === 'missed') {
         return events.slice().sort((a, b) => b.fire_at - a.fire_at);
      }
      return events.slice().sort((a, b) => {
         /* Ringing pins to top */
         if (a.status === 'ringing' && b.status !== 'ringing') return -1;
         if (b.status === 'ringing' && a.status !== 'ringing') return 1;
         return (a.effective_fire_at || a.fire_at) - (b.effective_fire_at || b.fire_at);
      });
   }

   /* =============================================================================
    * Render
    * ============================================================================= */

   function renderStats() {
      let active = 0,
         snoozed = 0,
         ringing = 0,
         missed = 0;
      state.events.forEach((e) => {
         if (e.status === 'pending') active++;
         else if (e.status === 'snoozed') snoozed++;
         else if (e.status === 'ringing') ringing++;
         else if (e.status === 'missed') missed++;
      });
      if (els.statActive) els.statActive.textContent = active;
      if (els.statSnoozed) els.statSnoozed.textContent = snoozed;
      if (els.statRinging) els.statRinging.textContent = ringing;
      if (els.statMissed) els.statMissed.textContent = missed;
   }

   function renderEmpty(tab, q) {
      if (q) {
         return (
            '<div class="sched-popover-empty-compact">No events match "' + escape(q) + '"</div>'
         );
      }
      if (tab === 'all') {
         return (
            '<div class="sched-popover-empty">' +
            '<svg class="sched-popover-empty-icon" viewBox="0 0 24 24" fill="none"' +
            ' stroke="currentColor" stroke-width="1.5" stroke-linecap="round"' +
            ' stroke-linejoin="round" aria-hidden="true">' +
            '<circle cx="12" cy="12" r="9"/>' +
            '<polyline points="12 7 12 12 15 14"/>' +
            '</svg>' +
            '<p class="sched-popover-empty-headline">Nothing scheduled</p>' +
            '<p class="sched-popover-empty-hint">' +
            'Try: <code>"set a 5 minute timer"</code><br>' +
            'or <code>"remind me at 7 PM"</code>' +
            '</p>' +
            '</div>'
         );
      }
      const tabLabel =
         tab === 'missed'
            ? 'missed events'
            : tab === 'timer'
              ? 'timers running'
              : tab === 'alarm'
                ? 'alarms scheduled'
                : tab === 'reminder'
                  ? 'reminders scheduled'
                  : tab === 'task'
                    ? 'tasks scheduled'
                    : tab === 'briefing'
                      ? 'briefings scheduled'
                      : 'events';
      return '<div class="sched-popover-empty-compact">No ' + escape(tabLabel) + '</div>';
   }

   function render() {
      if (!els.list) return;
      renderStats();

      const filtered = state.events.filter(
         (e) => eventMatchesTab(e, state.activeTab) && eventMatchesSearch(e, state.searchQuery)
      );
      const sorted = sortForTab(filtered, state.activeTab);

      if (sorted.length === 0) {
         els.list.innerHTML = renderEmpty(state.activeTab, state.searchQuery);
         return;
      }

      els.list.innerHTML = sorted.map(buildRowHTML).join('');

      /* Preserve the inline-confirm pill across re-renders.  An inbound
       * scheduler_events_changed broadcast triggers a full render() — without
       * this, the trash icon visually reverts to its idle state while
       * state.armedRowId still matches and the 3s timeout still ticks,
       * leaving an inconsistent UI mid-confirm. */
      if (state.armedRowId !== null) applyArmedPillContent();

      /* Track ringing IDs so we can announce transitions via aria-live. */
      const nowRinging = new Set();
      state.events.forEach((e) => {
         if (e.status === 'ringing') nowRinging.add(e.id);
      });
      nowRinging.forEach((id) => {
         if (!state.lastRingingIds.has(id)) {
            const ev = state.events.find((e) => e.id === id);
            if (ev && els.srAnnounce) {
               els.srAnnounce.textContent =
                  (ev.name || TYPE_LABEL[ev.event_type] || 'Event') + ' is ringing';
            }
         }
      });
      state.lastRingingIds = nowRinging;
   }

   function updateIndicator() {
      if (!els.btn) return;
      let hasRinging = false;
      let hasMissed = false;
      state.events.forEach((e) => {
         if (e.status === 'ringing') hasRinging = true;
         if (e.status === 'missed') hasMissed = true;
      });
      if (hasRinging || hasMissed) {
         els.btn.classList.add('has-attention');
      } else {
         els.btn.classList.remove('has-attention');
      }
      if (hasRinging) {
         els.btn.classList.add('has-ringing');
      } else {
         els.btn.classList.remove('has-ringing');
      }
      /* aria-label reflects counts for screen-reader users */
      let label = 'Open scheduler queue';
      const ringingCount = state.events.filter((e) => e.status === 'ringing').length;
      const missedCount = state.events.filter((e) => e.status === 'missed').length;
      if (ringingCount || missedCount) {
         const bits = [];
         if (ringingCount) bits.push(ringingCount + ' ringing');
         if (missedCount) bits.push(missedCount + ' missed');
         label += ' (' + bits.join(', ') + ')';
      }
      els.btn.setAttribute('aria-label', label);
   }

   /* =============================================================================
    * Network
    * =========================================================================== */

   function sendList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.send) return;
      DawnWS.send({ type: 'scheduler_action', payload: { action: 'list' } });
   }

   function sendCancel(eventId, occurrenceOnly) {
      if (typeof DawnWS === 'undefined' || !DawnWS.send) return;
      DawnWS.send({
         type: 'scheduler_action',
         payload: {
            action: occurrenceOnly ? 'cancel_occurrence' : 'cancel',
            event_id: eventId,
         },
      });
   }

   function sendClearMissed(eventId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.send) return;
      DawnWS.send({
         type: 'scheduler_action',
         payload: { action: 'clear_missed', event_id: eventId },
      });
   }

   function handleListResponse(payload) {
      if (!payload) return;
      state.events = Array.isArray(payload.events) ? payload.events : [];
      updateIndicator();
      if (state.isOpen) render();
   }

   function refresh() {
      sendList();
   }

   /* =============================================================================
    * Inline-confirm flow
    * ============================================================================= */

   function clearArm() {
      if (state.armedTimer) {
         clearTimeout(state.armedTimer);
         state.armedTimer = null;
      }
      if (state.armedRowId !== null) {
         /* Re-render to disarm the row's visual state */
         state.armedRowId = null;
         if (state.isOpen) render();
         announce('Cancel discarded');
      }
   }

   function announce(text) {
      if (els.srAnnounce) els.srAnnounce.textContent = text;
   }

   function armRow(eventId, nameText) {
      /* Single-armed invariant — arming row B disarms row A */
      if (state.armedRowId !== null && state.armedRowId !== eventId) {
         clearTimeout(state.armedTimer);
         state.armedTimer = null;
      }
      state.armedRowId = eventId;
      state.armedTimer = setTimeout(() => {
         clearArm();
      }, 3000);
      render();
      announce('Confirm cancel ' + (nameText || ''));
   }

   function applyArmedPillContent() {
      if (state.armedRowId === null) return;
      const sel = els.list.querySelector(
         '.sched-row[data-event-id="' + state.armedRowId + '"] .sched-row-delete'
      );
      if (!sel) return;
      sel.classList.add('armed');
      const label = sel.getAttribute('data-delete-label') || 'Cancel';
      sel.textContent = '× ' + label + '?';
   }

   /* =============================================================================
    * Recurring-cancel disambiguation popup
    * ============================================================================= */

   function closeMenu() {
      const existing = els.popover.querySelector('.sched-recurring-menu');
      if (existing) existing.remove();
      state.menuOpenForId = null;
   }

   function openRecurringMenu(rowEl, eventId, nameText) {
      closeMenu();
      const menu = document.createElement('div');
      menu.className = 'dawn-split-menu anchor-right below sched-recurring-menu';
      menu.setAttribute('role', 'menu');
      menu.innerHTML =
         '<button type="button" class="dropdown-item" role="menuitem"' +
         ' data-cancel-mode="occurrence">Cancel this occurrence</button>' +
         '<button type="button" class="dropdown-item" role="menuitem"' +
         ' data-cancel-mode="series">Cancel the series</button>';

      /* Anchor the menu to the trash button via inline position. */
      const trash = rowEl.querySelector('.sched-row-delete');
      if (!trash) return;
      const actions = rowEl.querySelector('.sched-row-actions');
      if (!actions) return;
      /* Make actions a relative anchor so the absolute menu lands beneath it */
      if (getComputedStyle(actions).position === 'static') {
         actions.style.position = 'relative';
      }
      actions.appendChild(menu);
      state.menuOpenForId = eventId;

      const onSelect = (mode) => {
         closeMenu();
         clearArm();
         /* Optimistic remove */
         removeRowOptimistic(eventId);
         sendCancel(eventId, mode === 'occurrence');
      };
      menu.addEventListener('click', (e) => {
         const btn = e.target.closest('[data-cancel-mode]');
         if (!btn) return;
         e.stopPropagation();
         onSelect(btn.getAttribute('data-cancel-mode'));
      });
   }

   function removeRowOptimistic(eventId) {
      const idx = state.events.findIndex((e) => e.id === eventId);
      if (idx >= 0) {
         state.events.splice(idx, 1);
         state.expandedRowIds.delete(eventId);
         updateIndicator();
         render();
      }
   }

   /* =============================================================================
    * Event delegation — clicks within the list and the popover
    * ============================================================================= */

   function onListClick(e) {
      /* Expand-toggle has priority over the disarm-on-empty-click default —
       * clicking the chevron should expand/collapse, not just disarm. */
      const expandTrigger = e.target.closest('[data-expand-trigger]');
      if (expandTrigger) {
         const row = expandTrigger.closest('.sched-row');
         if (row) {
            const eventId = parseInt(row.getAttribute('data-event-id'), 10);
            if (state.expandedRowIds.has(eventId)) {
               state.expandedRowIds.delete(eventId);
            } else {
               state.expandedRowIds.add(eventId);
            }
            e.stopPropagation();
            render();
            return;
         }
      }

      const trashBtn = e.target.closest('.sched-row-delete');
      if (!trashBtn) {
         /* Click was elsewhere in the list — disarm any pending row */
         clearArm();
         return;
      }
      const row = trashBtn.closest('.sched-row');
      if (!row) return;
      const eventId = parseInt(row.getAttribute('data-event-id'), 10);
      const status = row.getAttribute('data-status');
      const recurrence = row.getAttribute('data-recurrence');
      const event = state.events.find((ev) => ev.id === eventId);
      const nameText = event ? event.name || '' : '';

      e.stopPropagation();

      if (status === 'ringing') {
         /* Send existing dismiss action — same handler used by banner */
         DawnWS.send({
            type: 'scheduler_action',
            payload: { action: 'dismiss', event_id: eventId },
         });
         removeRowOptimistic(eventId);
         return;
      }
      if (status === 'missed') {
         /* Server-side clear: flips status='missed' → 'dismissed' so the row
          * stops surfacing in the panel.  Without this, a local-only remove
          * would come back on the next scheduler_events_changed refresh and
          * re-light the dot. */
         removeRowOptimistic(eventId);
         sendClearMissed(eventId);
         return;
      }

      /* Pending / snoozed: inline-confirm flow */
      if (recurrence && recurrence !== 'once') {
         /* Skip inline-confirm — open the disambiguation menu directly */
         openRecurringMenu(row, eventId, nameText);
         return;
      }

      if (state.armedRowId === eventId) {
         /* Second click → confirm */
         const id = eventId;
         clearArm();
         removeRowOptimistic(id);
         sendCancel(id, false);
         return;
      }
      armRow(eventId, nameText);
      /* After re-render, set the armed pill content */
      applyArmedPillContent();
   }

   function onListKeydown(e) {
      if (e.key === 'Escape') {
         if (state.menuOpenForId !== null) {
            closeMenu();
            e.stopPropagation();
            return;
         }
         if (state.armedRowId !== null) {
            clearArm();
            e.stopPropagation();
            return;
         }
      }
      if (e.key === 'Delete' || e.key === 'Backspace' || e.key === 'Enter') {
         const row = e.target.closest('.sched-row');
         /* Enter on a child element (e.g. the trash button) already fires
          * the button's click handler — don't double-fire.  Only intercept
          * Enter when focus is on the LI itself.  Delete/Backspace are
          * row-level shortcuts regardless of inner focus. */
         if (row && (e.key !== 'Enter' || e.target === row)) {
            const trash = row.querySelector('.sched-row-delete');
            if (trash) {
               e.preventDefault();
               trash.click();
            }
         }
      }
   }

   function onDocClick(e) {
      if (!state.isOpen) return;
      /* Click outside the popover entirely → close it */
      if (
         els.popover &&
         !els.popover.contains(e.target) &&
         e.target !== els.btn &&
         (!els.btn || !els.btn.contains(e.target))
      ) {
         close();
         return;
      }
      /* Click inside the popover but outside any armed row / open menu → disarm */
      if (state.armedRowId !== null) {
         const armedRow = els.list.querySelector(
            '.sched-row[data-event-id="' + state.armedRowId + '"]'
         );
         if (armedRow && !armedRow.contains(e.target)) {
            clearArm();
         }
      }
      if (state.menuOpenForId !== null) {
         const openMenu = els.popover.querySelector('.sched-recurring-menu');
         if (openMenu && !openMenu.contains(e.target)) {
            closeMenu();
         }
      }
      if (state.clearMissedArmed && e.target !== els.clearMissedBtn) {
         disarmClearMissed();
      }
   }

   /* =============================================================================
    * Clear-all-missed footer (inline-confirm pattern)
    * ============================================================================= */

   function disarmClearMissed() {
      if (state.clearMissedTimer) {
         clearTimeout(state.clearMissedTimer);
         state.clearMissedTimer = null;
      }
      state.clearMissedArmed = false;
      if (els.clearMissedBtn) {
         els.clearMissedBtn.classList.remove('armed');
         els.clearMissedBtn.textContent = 'Clear all missed';
      }
   }

   function onClearMissedClick(e) {
      e.stopPropagation();
      const missed = state.events.filter((ev) => ev.status === 'missed');
      if (missed.length === 0) return;
      if (!state.clearMissedArmed) {
         state.clearMissedArmed = true;
         els.clearMissedBtn.classList.add('armed');
         els.clearMissedBtn.textContent = 'Confirm clear (' + missed.length + ')?';
         state.clearMissedTimer = setTimeout(disarmClearMissed, 3000);
         return;
      }
      /* Confirmed — server-side clear for each missed event so the dot
       * actually goes away (local-only removal would return on next refetch). */
      missed.forEach((ev) => {
         removeRowOptimistic(ev.id);
         sendClearMissed(ev.id);
      });
      disarmClearMissed();
   }

   /* =============================================================================
    * Tab + search wiring
    * ============================================================================= */

   function setTab(tab) {
      if (state.activeTab === tab) return;
      state.activeTab = tab;
      clearArm();
      if (els.tabs) {
         els.tabs.forEach((t) => {
            const isActive = t.getAttribute('data-tab') === tab;
            t.classList.toggle('active', isActive);
            t.setAttribute('aria-selected', isActive ? 'true' : 'false');
         });
      }
      render();
   }

   function onSearchInput(e) {
      state.searchQuery = (e.target.value || '').trim();
      render();
   }

   /* =============================================================================
    * Countdown ticker — only runs while popover is open
    * ============================================================================= */

   function startCountdown() {
      stopCountdown();
      state.countdownTimer = setInterval(() => {
         /* Only re-render time columns if there's at least one short-fuse
          * timer in the visible set.  Skip otherwise to avoid pointless
          * DOM churn. */
         const needsTick = state.events.some(
            (e) =>
               e.event_type === 'timer' &&
               isActiveStatus(e.status) &&
               (e.effective_fire_at || e.fire_at) - Math.floor(Date.now() / 1000) < 3600
         );
         if (needsTick && state.isOpen) {
            /* Only update time columns in-place — avoids tearing down the
             * armed pill or open menu. */
            updateTimeColumns();
         }
      }, 1000);
   }

   function stopCountdown() {
      if (state.countdownTimer) {
         clearInterval(state.countdownTimer);
         state.countdownTimer = null;
      }
   }

   function updateTimeColumns() {
      if (!els.list) return;
      const rows = els.list.querySelectorAll('.sched-row');
      rows.forEach((row) => {
         const eventId = parseInt(row.getAttribute('data-event-id'), 10);
         const event = state.events.find((e) => e.id === eventId);
         if (!event) return;
         const t = row.querySelector('.sched-row-time');
         if (t) t.textContent = formatTimeColumn(event);
         const when = row.querySelector('.sched-row-when');
         if (when && event.status !== 'ringing' && event.status !== 'missed') {
            when.textContent = formatWhen(event);
         }
      });
   }

   /* =============================================================================
    * Open / close / toggle
    * ============================================================================= */

   function open() {
      if (!els.popover) return;
      state.triggerEl = document.activeElement;
      els.popover.classList.remove('hidden');
      if (els.btn) els.btn.setAttribute('aria-expanded', 'true');
      state.isOpen = true;
      sendList();
      startCountdown();
      /* Defer focus so the popover is in the layout when we focus the close
       * button — avoids triggering the focus-back-to-trigger fallback. */
      setTimeout(() => {
         if (els.closeBtn) els.closeBtn.focus();
      }, 0);
   }

   function close() {
      if (!els.popover) return;
      els.popover.classList.add('hidden');
      if (els.btn) els.btn.setAttribute('aria-expanded', 'false');
      state.isOpen = false;
      clearArm();
      disarmClearMissed();
      closeMenu();
      stopCountdown();
      /* Focus return: back to the trigger button if it's the canonical
       * source, otherwise leave focus where it was. */
      if (els.btn && (!state.triggerEl || state.triggerEl === document.body)) {
         els.btn.focus();
      } else if (state.triggerEl && document.contains(state.triggerEl)) {
         state.triggerEl.focus();
      }
   }

   function toggle() {
      if (state.isOpen) close();
      else open();
   }

   /* =============================================================================
    * Visibility (auth-gated header button)
    * ============================================================================= */

   function updateVisibility(authState) {
      if (!els.btn) return;
      const ok = authState && authState.authenticated;
      els.btn.classList.toggle('hidden', !ok);
      if (!ok && state.isOpen) close();
   }

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init() {
      els.btn = document.getElementById('scheduler-btn');
      els.popover = document.getElementById('scheduler-popover');
      els.closeBtn = document.getElementById('scheduler-close');
      els.list = document.getElementById('sched-popover-list');
      els.searchInput = document.getElementById('sched-popover-search-input');
      els.statActive = document.getElementById('sched-stat-active');
      els.statSnoozed = document.getElementById('sched-stat-snoozed');
      els.statRinging = document.getElementById('sched-stat-ringing');
      els.statMissed = document.getElementById('sched-stat-missed');
      els.clearMissedBtn = document.getElementById('sched-clear-missed');
      els.srAnnounce = document.getElementById('sched-popover-sr-announce');
      els.tabs = els.popover ? Array.from(els.popover.querySelectorAll('.sched-popover-tab')) : [];

      if (!els.btn || !els.popover) return;

      els.btn.addEventListener('click', toggle);
      if (els.closeBtn) els.closeBtn.addEventListener('click', close);
      if (els.searchInput) els.searchInput.addEventListener('input', onSearchInput);
      if (els.clearMissedBtn) els.clearMissedBtn.addEventListener('click', onClearMissedClick);

      els.tabs.forEach((t) => {
         t.addEventListener('click', () => setTab(t.getAttribute('data-tab')));
      });

      if (els.list) {
         els.list.addEventListener('click', onListClick);
         els.list.addEventListener('keydown', onListKeydown);
      }

      /* Escape inside the popover closes it */
      els.popover.addEventListener('keydown', (e) => {
         if (e.key === 'Escape' && state.armedRowId === null && state.menuOpenForId === null) {
            close();
         }
      });

      state.docClickHandler = onDocClick;
      document.addEventListener('click', state.docClickHandler, true);

      /* Initial fetch if already authenticated.  If not, the auth event will
       * trigger updateVisibility + the next dawn.js init pass calls refresh. */
      if (
         typeof DawnState !== 'undefined' &&
         DawnState.authState &&
         DawnState.authState.authenticated
      ) {
         sendList();
      }
   }

   window.DawnSchedulerQueue = {
      init,
      open,
      close,
      toggle,
      refresh,
      handleListResponse,
      updateVisibility,
   };
})();
