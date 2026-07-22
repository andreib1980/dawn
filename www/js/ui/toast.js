/**
 * DAWN Toast Notifications Module
 * Simple toast notification system with auto-dismiss
 *
 * Usage:
 *   DawnToast.show('Message here')           // Default 'info' type
 *   DawnToast.show('Error!', 'error')        // Error toast
 *   DawnToast.show('Success!', 'success')    // Success toast
 *   DawnToast.show('Warning', 'warning', 8000, {actions: [{label: 'Fix', onClick: fn}]})
 *   DawnToast.showUndo('Attachment removed.', {onUndo: restore, onExpire: commit})
 *
 * options (show): {actions:[{label,onClick}], badge:'TEXT', assertive:bool,
 *                  onDismiss: fn}  — onDismiss fires once on any dismissal.
 * All toasts support swipe-to-dismiss (pointer/touch) in addition to
 * click / Enter / Escape.
 */
(function (global) {
   'use strict';

   /**
    * Show a toast notification
    * @param {string} message - Text to display
    * @param {string} type - Toast type: 'info', 'success', 'error', 'warning'
    * @param {number} duration - Auto-dismiss in ms (default 4000, 0 = manual)
    * @param {object} options - Optional: {actions: [{label, onClick}], badge: 'TEXT'}
    * @returns {HTMLElement} The toast element (for attaching event listeners)
    */
   function showToast(message, type = 'info', duration = 4000, options = {}) {
      // Create toast container if it doesn't exist
      let container = document.getElementById('toast-container');
      if (!container) {
         container = document.createElement('div');
         container.id = 'toast-container';
         document.body.appendChild(container);
      }

      const toast = document.createElement('div');
      toast.className = 'toast toast-' + type;
      // Optional leading badge chip (e.g. 'ATTENTION'); built via DOM so both the
      // badge and the message stay textContent (no innerHTML — XSS-safe).
      if (options.badge) {
         const badge = document.createElement('span');
         // Compose the shared .dawn-badge primitive (shape/typography); .toast-badge
         // only tints it. No per-component re-implementation of the chip.
         badge.className = 'dawn-badge toast-badge';
         badge.textContent = options.badge;
         toast.appendChild(badge);
         const text = document.createElement('span');
         text.className = 'toast-text';
         text.textContent = message;
         toast.appendChild(text);
      } else {
         toast.textContent = message;
      }

      // Append action buttons via DOM construction (no innerHTML).  Stop the
      // click bubbling so an action press doesn't also trigger dismiss-on-click.
      if (options.actions) {
         options.actions.forEach((action) => {
            const btn = document.createElement('button');
            btn.className = 'toast-action';
            btn.textContent = action.label;
            btn.addEventListener('click', (e) => {
               e.stopPropagation();
               action.onClick(e);
            });
            toast.appendChild(btn);
         });
      }

      // ARIA: interrupt the screen-reader user only for genuinely urgent toasts
      // (errors, and callers that opt in via options.assertive — e.g. a spoken
      // ATTENTION alert).  Ambient/status toasts stay polite so they don't cut
      // across whatever the user is doing.
      if (type === 'error' || options.assertive) {
         toast.setAttribute('role', 'alert');
         toast.setAttribute('aria-live', 'assertive');
      } else {
         toast.setAttribute('role', 'status');
         toast.setAttribute('aria-live', 'polite');
      }

      container.appendChild(toast);

      let dismissTimer = null;
      let dismissed = false;
      const dismiss = () => {
         if (dismissed) return; // idempotent — swipe + timer can both fire
         dismissed = true;
         clearTimeout(dismissTimer);
         toast.classList.add('toast-fade-out');
         setTimeout(() => toast.remove(), 300);
         if (typeof options.onDismiss === 'function') options.onDismiss();
      };
      // Expose so callers (e.g. showUndo's action) can dismiss with the fade.
      toast.dismiss = dismiss;

      if (duration > 0) {
         const startDismissTimer = () => {
            dismissTimer = setTimeout(dismiss, duration);
         };
         // Pause timer on hover/focus for accessibility (H10).  focusin/focusout
         // bubble (unlike focus/blur), so the timer stays paused while focus is
         // anywhere inside the toast — including on the Undo action button.
         toast.addEventListener('mouseenter', () => clearTimeout(dismissTimer));
         toast.addEventListener('mouseleave', startDismissTimer);
         toast.addEventListener('focusin', () => clearTimeout(dismissTimer));
         toast.addEventListener('focusout', startDismissTimer);
         startDismissTimer();
      }

      // Swipe-to-dismiss (pointer/touch).  A horizontal drag past the threshold
      // dismisses; a shorter drag snaps back.  suppressNextClick stops the
      // trailing synthesized click from also firing dismiss.  Reduced-motion
      // skips the follow-the-finger transform but keeps threshold dismissal.
      const reduceMotion =
         window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
      let startX = 0;
      let dx = 0;
      let dragging = false;
      let suppressNextClick = false;
      const SWIPE_DRAG_START = 8; // px before a move counts as a drag
      const SWIPE_DISMISS_AT = 80; // px past which release dismisses

      toast.addEventListener('pointerdown', (e) => {
         // Never start a swipe from an action button — keep it tappable.
         if (e.target.closest && e.target.closest('.toast-action')) return;
         startX = e.clientX;
         dx = 0;
         dragging = true;
         try {
            toast.setPointerCapture(e.pointerId);
         } catch (err) {
            /* pointer capture unsupported — swipe still works */
         }
      });
      toast.addEventListener('pointermove', (e) => {
         if (!dragging) return;
         dx = e.clientX - startX;
         if (Math.abs(dx) > SWIPE_DRAG_START) suppressNextClick = true;
         if (suppressNextClick && !reduceMotion) {
            toast.style.transform = 'translateX(' + dx + 'px)';
            toast.style.opacity = String(Math.max(0, 1 - Math.abs(dx) / 200));
         }
      });
      const endSwipe = () => {
         if (!dragging) return;
         dragging = false;
         if (Math.abs(dx) > SWIPE_DISMISS_AT) {
            dismiss();
         } else {
            // Snap back to resting position.
            toast.style.transform = '';
            toast.style.opacity = '';
         }
      };
      toast.addEventListener('pointerup', endSwipe);
      toast.addEventListener('pointercancel', endSwipe);

      // The pointer cursor promises dismissal — wire it (click, or Enter/Escape
      // for keyboard users on the focusable toast).  A completed drag suppresses
      // the trailing click so a swipe doesn't double-fire dismiss.
      toast.addEventListener('click', () => {
         if (suppressNextClick) {
            suppressNextClick = false;
            return;
         }
         dismiss();
      });
      toast.addEventListener('keydown', (e) => {
         // Escape always dismisses; Enter only when the toast itself is focused
         // (so Enter on an action button runs the action, not a dismiss).
         if (e.key === 'Escape' || (e.key === 'Enter' && e.target === toast)) {
            dismiss();
         }
      });

      // Make focusable for keyboard users (M11)
      toast.setAttribute('tabindex', '0');

      return toast;
   }

   /**
    * Show an "undo" toast for an optimistic, CLIENT-reversible action.
    * The action should already be applied optimistically; `onUndo` reverts it,
    * `onExpire` (optional) finalizes it if the toast is dismissed without
    * undoing.  Kept polite (role=status) and NOT auto-focused so it never steals
    * the composer; the auto-dismiss timer pauses once the user focuses the toast
    * or its Undo button.  Do NOT use for irreversible server-side deletes.
    * @param {string} message - e.g. "Attachment removed."
    * @param {object} opts - {onUndo, onExpire, duration=8000, label='Undo'}
    * @returns {HTMLElement}
    */
   function showUndo(message, opts = {}) {
      const { onUndo, onExpire, duration = 8000, label = 'Undo' } = opts;
      let undone = false;
      const toast = showToast(message, 'info', duration, {
         actions: [
            {
               label: label,
               onClick: () => {
                  undone = true;
                  if (typeof onUndo === 'function') onUndo();
                  toast.dismiss();
               },
            },
         ],
         onDismiss: () => {
            if (!undone && typeof onExpire === 'function') onExpire();
         },
      });
      return toast;
   }

   // Expose globally
   global.DawnToast = {
      show: showToast,
      showUndo: showUndo,
   };
})(window);
