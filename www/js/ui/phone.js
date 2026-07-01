/*
 * Phone call UI: incoming-call banner + persistent in-call panel.
 *
 * On a `phone_call_notification` (status ringing/active/ended):
 *  - ringing: a top-center banner with Answer / Reject.
 *  - active:  a PERSISTENT in-call panel (caller, live duration, Hang Up,
 *             Minimize). It has no close affordance — it can only be dismissed
 *             by hanging up (or, if the daemon state is lost, by long-pressing /
 *             right-clicking the minimized pill to clear stale UI locally).
 *  - ended:   clears both.
 * Both Answer/Reject/Hang Up send a phone_action WS message. On (re)connect the
 * client requests a phone_status snapshot so an active call rehydrates the panel;
 * on disconnect the panel is torn down (it must not outlive its connection).
 */
(function () {
   'use strict';

   let container = null;
   let ringCtx = null;
   let ringTimer = null;
   let answeringTimeout = null;

   let callPanel = null;
   let durationTimer = null;
   let hangupFallback = null;
   let callBaseElapsedMs = 0;
   let callAnchorMs = 0;
   let announcer = null;
   let prevFocus = null;

   /* Accept only concrete raster image types for the caller photo data-URI. */
   const PHOTO_MIME_RE = /^image\/(png|jpe?g|gif|webp)$/;

   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined' && DawnFormat.escapeHtml) {
         return DawnFormat.escapeHtml(str);
      }
      return String(str == null ? '' : str).replace(
         /[&<>"']/g,
         (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]
      );
   }

   function ensureContainer() {
      if (container) return container;
      container = document.createElement('div');
      container.id = 'phone-notifications';
      document.body.appendChild(container);
      return container;
   }

   /* Looping ring tone via Web Audio (a two-tone burst, then a gap) so a remote
    * browser isn't silent when the physical ring is only at the Jetson. */
   function startRing() {
      if (ringTimer) return;
      try {
         if (!ringCtx) {
            ringCtx = new (window.AudioContext || window.webkitAudioContext)();
         }
         if (ringCtx.state === 'suspended') ringCtx.resume();
      } catch (e) {
         console.warn('Phone: could not init audio context:', e);
         return;
      }

      function burst() {
         try {
            if (!ringCtx || ringCtx.state === 'closed') return;
            const now = ringCtx.currentTime;
            [440, 480].forEach((freq, i) => {
               const osc = ringCtx.createOscillator();
               const gain = ringCtx.createGain();
               osc.connect(gain);
               gain.connect(ringCtx.destination);
               osc.frequency.value = freq;
               osc.type = 'sine';
               const t0 = now + i * 0.45;
               gain.gain.setValueAtTime(0.0001, t0);
               gain.gain.exponentialRampToValueAtTime(0.22, t0 + 0.02);
               gain.gain.exponentialRampToValueAtTime(0.0001, t0 + 0.4);
               osc.start(t0);
               osc.stop(t0 + 0.42);
            });
         } catch (e) {
            /* context may have closed mid-loop — ignore */
         }
      }

      burst();
      ringTimer = setInterval(burst, 3000);
   }

   function stopRing() {
      if (ringTimer) {
         clearInterval(ringTimer);
         ringTimer = null;
      }
   }

   function clearBanner() {
      stopRing();
      if (answeringTimeout) {
         clearTimeout(answeringTimeout);
         answeringTimeout = null;
      }
      if (!container) return;
      const existing = container.querySelector('.phone-banner');
      if (existing) {
         existing.classList.add('phone-banner-out');
         setTimeout(() => existing.remove(), 200);
      }
   }

   function sendAction(action) {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'phone_action', payload: { action } });
      }
   }

   /* Visually-hidden polite live region for one-shot state announcements
    * ("Call connected" / "Call ended") — the panel itself is NOT a live region,
    * so the per-second duration never gets announced. */
   function ensureAnnouncer() {
      if (announcer) return announcer;
      announcer = document.createElement('div');
      announcer.className = 'sr-only';
      announcer.setAttribute('aria-live', 'polite');
      document.body.appendChild(announcer);
      return announcer;
   }

   function announce(msg) {
      const a = ensureAnnouncer();
      a.textContent = '';
      // Re-set on a later tick so an identical consecutive message re-announces.
      setTimeout(() => {
         a.textContent = msg;
      }, 50);
   }

   function formatDuration(ms) {
      const total = Math.max(0, Math.round(ms / 1000));
      const h = Math.floor(total / 3600);
      const m = Math.floor((total % 3600) / 60);
      const s = total % 60;
      const pad = (n) => String(n).padStart(2, '0');
      return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${pad(m)}:${pad(s)}`;
   }

   /* Duration is computed from a stored anchor each tick (not an incrementing
    * counter) so it stays accurate through background-tab throttling and
    * reconnects; the server supplies the authoritative elapsed baseline. */
   function updateDuration() {
      if (!callPanel) return;
      const txt = formatDuration(callBaseElapsedMs + (Date.now() - callAnchorMs));
      callPanel.querySelectorAll('.js-phone-dur').forEach((el) => {
         el.textContent = txt;
      });
   }

   function clearCallPanel() {
      if (durationTimer) {
         clearInterval(durationTimer);
         durationTimer = null;
      }
      if (hangupFallback) {
         clearTimeout(hangupFallback);
         hangupFallback = null;
      }
      if (callPanel) {
         /* Restore focus to wherever it was before the call took it, so tearing
          * down the focused panel/pill doesn't drop focus to <body>. */
         const hadFocus = callPanel.contains(document.activeElement);
         callPanel.remove();
         callPanel = null;
         if (hadFocus && prevFocus && document.body.contains(prevFocus)) {
            try {
               prevFocus.focus();
            } catch (e) {
               /* element no longer focusable — ignore */
            }
         }
         prevFocus = null;
      }
   }

   function minimizeCall() {
      if (!callPanel) return;
      callPanel.classList.add('phone-minimized');
      const pill = callPanel.querySelector('.phone-callbar-pill');
      if (pill) pill.focus();
   }

   function expandCall() {
      if (!callPanel) return;
      callPanel.classList.remove('phone-minimized');
      callPanel.focus();
   }

   function hangUp() {
      sendAction('hangup');
      /* Don't remove the panel optimistically — the click can be dropped by the
       * server's single-in-flight guard, and the authoritative teardown is the
       * 'ended' frame. Fall back to a local clear if that never arrives. */
      if (hangupFallback) clearTimeout(hangupFallback);
      hangupFallback = setTimeout(clearCallPanel, 30000);
   }

   function showActive(payload) {
      clearBanner(); // remove the ringing banner (separate element)

      const name = payload.name || '';
      const number = payload.number || '';
      const display = name || number || 'Unknown caller';

      callBaseElapsedMs = (payload.elapsed_sec || 0) * 1000;
      callAnchorMs = Date.now();

      if (!callPanel) {
         prevFocus = document.activeElement;
         callPanel = document.createElement('div');
         callPanel.id = 'phone-call-panel';
         /* Attach the container-level Escape handler ONCE — it survives the
          * per-call innerHTML rebuilds (child button listeners are re-wired
          * below), so it never accrues duplicates on a repeated active frame. */
         callPanel.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && !callPanel.classList.contains('phone-minimized')) {
               e.preventDefault();
               minimizeCall();
            }
         });
         document.body.appendChild(callPanel);
      }
      // Rebuild expanded each call — never inherit a prior call's minimized state.
      callPanel.className = 'phone-callbar';
      callPanel.setAttribute('role', 'group');
      callPanel.setAttribute('aria-label', `Call in progress with ${display}`);
      callPanel.tabIndex = -1;

      const photo = payload.photo;
      const hasPhoto =
         photo && typeof photo.data === 'string' && PHOTO_MIME_RE.test(photo.mime || '');
      const photoHtml = hasPhoto
         ? '<div class="phone-photo"><img alt="" /></div>'
         : '<div class="phone-photo" aria-hidden="true">📞</div>';

      callPanel.innerHTML = `
         <div class="phone-callbar-full">
            ${photoHtml}
            <div class="phone-info">
               <span class="phone-badge phone-badge-active"><span aria-hidden="true">📞</span> On call</span>
               <span class="phone-name">${escapeHtml(display)}</span>
               <span class="phone-duration js-phone-dur" aria-hidden="true">00:00</span>
            </div>
            <div class="phone-actions">
               <button class="phone-btn phone-btn-minimize" aria-label="Minimize call window" title="Minimize">&#8211;</button>
               <button class="phone-btn phone-btn-hangup" title="Hang up">Hang Up</button>
            </div>
         </div>
         <button
            class="phone-callbar-pill"
            aria-label="Call in progress — activate to expand"
            title="Tap to expand · long-press to dismiss">
            <span aria-hidden="true">📞</span>
            <span class="phone-pill-duration js-phone-dur" aria-hidden="true">00:00</span>
         </button>`;

      if (hasPhoto) {
         const img = callPanel.querySelector('.phone-photo img');
         if (img) img.src = `data:${photo.mime};base64,${photo.data}`;
      }

      callPanel.querySelector('.phone-btn-hangup').addEventListener('click', hangUp);
      callPanel.querySelector('.phone-btn-minimize').addEventListener('click', minimizeCall);
      const pill = callPanel.querySelector('.phone-callbar-pill');
      pill.addEventListener('click', expandCall);
      /* Force-dismiss stale UI (right-click / long-press) — clears locally only,
       * never sends a hangup. The escape hatch for a lost 'ended' event. */
      pill.addEventListener('contextmenu', (e) => {
         e.preventDefault();
         clearCallPanel();
      });

      updateDuration();
      if (durationTimer) clearInterval(durationTimer);
      durationTimer = setInterval(updateDuration, 1000);

      // Move focus to the panel container (NOT Hang Up — a stray Enter would drop
      // the call). requestAnimationFrame lets the slide-in class apply first.
      requestAnimationFrame(() => {
         callPanel.classList.add('phone-callbar-in');
         callPanel.focus();
      });
      announce('Call connected');
   }

   function showRinging(payload) {
      const el = ensureContainer();
      clearBanner();

      const name = payload.name || '';
      const number = payload.number || '';
      const display = name || number || 'Unknown caller';

      const banner = document.createElement('div');
      banner.className = 'phone-banner phone-ringing';
      /* role=alert + aria-live let the SR announce the banner's own content
       * (badge + name + number); no aria-label, which would go stale after the
       * "Answering…" flip and can suppress the button subtree in some readers. */
      banner.setAttribute('role', 'alert');
      banner.setAttribute('aria-live', 'assertive');

      /* Photo (optional) — set via img.src property, never interpolated into
       * markup. Only accept image/* MIME to avoid a data-URI surprise. */
      let photoHtml = '<div class="phone-photo" aria-hidden="true">📞</div>';
      const photo = payload.photo;
      const hasPhoto =
         photo && typeof photo.data === 'string' && PHOTO_MIME_RE.test(photo.mime || '');
      if (hasPhoto) {
         photoHtml = '<div class="phone-photo"><img alt="" /></div>';
      }

      banner.innerHTML = `
         ${photoHtml}
         <div class="phone-info">
            <span class="phone-badge"><span aria-hidden="true">📞</span> Incoming call</span>
            <span class="phone-name">${escapeHtml(display)}</span>
            ${name && number ? `<span class="phone-number">${escapeHtml(number)}</span>` : ''}
         </div>
         <div class="phone-actions">
            <button class="phone-btn phone-btn-answer" title="Answer">Answer</button>
            <button class="phone-btn phone-btn-reject" title="Reject">Reject</button>
         </div>`;

      if (hasPhoto) {
         const img = banner.querySelector('.phone-photo img');
         if (img) img.src = `data:${photo.mime};base64,${photo.data}`;
      }

      const answerBtn = banner.querySelector('.phone-btn-answer');
      const rejectBtn = banner.querySelector('.phone-btn-reject');
      answerBtn.addEventListener('click', () => {
         sendAction('answer');
         stopRing();
         answerBtn.disabled = true;
         rejectBtn.disabled = true;
         const nm = banner.querySelector('.phone-badge');
         if (nm) nm.textContent = 'Answering…';
         /* The 'active' notification clears the banner once connected; this
          * fallback recovers if that event never arrives (call-setup failure). */
         if (answeringTimeout) clearTimeout(answeringTimeout);
         answeringTimeout = setTimeout(clearBanner, 30000);
      });
      rejectBtn.addEventListener('click', () => {
         sendAction('reject');
         clearBanner();
      });

      el.appendChild(banner);
      /* Slide in and move focus to Answer — a ringing call is a priority action
       * and the banner mounts at the end of the tab order otherwise. */
      requestAnimationFrame(() => {
         banner.classList.add('phone-banner-in');
         answerBtn.focus();
      });
      startRing();
   }

   function handleNotification(payload) {
      if (!payload) return;
      const status = payload.status || 'ringing';
      if (status === 'ringing') {
         clearCallPanel();
         showRinging(payload);
      } else if (status === 'active') {
         showActive(payload); // also clears the ringing banner
      } else {
         /* ended */
         clearBanner();
         if (callPanel) announce('Call ended');
         clearCallPanel();
      }
   }

   /* On (re)connect, ask the daemon whether a call is currently active so the
    * panel rehydrates (e.g. after a page reload mid-call). */
   function handleReconnect() {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send({ type: 'phone_status' });
      }
   }

   /* On disconnect, tear down the call UI — its state is no longer authoritative
    * and a stale "On call" pill with no dismiss would be a trap. Reconnect
    * rehydrates via handleReconnect(). */
   function handleDisconnect() {
      clearBanner();
      clearCallPanel();
   }

   window.addEventListener('beforeunload', () => {
      if (durationTimer) clearInterval(durationTimer);
      if (ringTimer) clearInterval(ringTimer);
      if (hangupFallback) clearTimeout(hangupFallback);
   });

   window.DawnPhone = { handleNotification, handleReconnect, handleDisconnect };
})();
