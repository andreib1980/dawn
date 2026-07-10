/**
 * DAWN Toast Notifications Module
 * Simple toast notification system with auto-dismiss
 *
 * Usage:
 *   DawnToast.show('Message here')           // Default 'info' type
 *   DawnToast.show('Error!', 'error')        // Error toast
 *   DawnToast.show('Success!', 'success')    // Success toast
 *   DawnToast.show('Warning', 'warning', 8000, {actions: [{label: 'Fix', onClick: fn}]})
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
      const dismiss = () => {
         clearTimeout(dismissTimer);
         toast.classList.add('toast-fade-out');
         setTimeout(() => toast.remove(), 300);
      };

      if (duration > 0) {
         const startDismissTimer = () => {
            dismissTimer = setTimeout(dismiss, duration);
         };
         // Pause timer on hover/focus for accessibility (H10)
         toast.addEventListener('mouseenter', () => clearTimeout(dismissTimer));
         toast.addEventListener('mouseleave', startDismissTimer);
         toast.addEventListener('focus', () => clearTimeout(dismissTimer));
         toast.addEventListener('blur', startDismissTimer);
         startDismissTimer();
      }

      // The pointer cursor promises dismissal — wire it (click, or Enter/Escape
      // for keyboard users on the focusable toast).
      toast.addEventListener('click', dismiss);
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

   // Expose globally
   global.DawnToast = {
      show: showToast,
   };
})(window);
