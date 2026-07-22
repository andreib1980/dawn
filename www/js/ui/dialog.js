/**
 * DAWN Dialog — Promise-based styled confirm / prompt / alert.
 *
 * Thin async wrappers over DawnSettingsModals.showConfirmModal / showInputModal
 * so call sites can `await` a themed dialog instead of threading callbacks —
 * the drop-in replacement for native confirm() / prompt() / alert().
 *
 *   if (await DawnDialog.confirm('Delete this?', { danger: true })) { ... }
 *   const name = await DawnDialog.prompt('New name?', current);   // string | null
 *   await DawnDialog.alert('Saved.');
 *
 * SERIALIZED: the underlying confirm/input modals share one DOM node and
 * single-slot module state, so opening a second dialog while one is pending
 * would clobber the first (its Promise would never resolve, and focus would
 * restore to the wrong element).  Every call runs through one queue — a new
 * dialog waits until the current one has fully closed.  This also makes the
 * shared modalTriggerElement correct: only one dialog is ever open, so a
 * dialog-opened-from-a-modal returns focus to that modal.
 *
 * Load AFTER js/ui/settings/modals.js.  Consumers call at runtime (event
 * handlers), so their earlier load position in index.html is fine.
 */
(function (global) {
   'use strict';

   // Tail of the serialization chain.  Errors are swallowed so one rejected
   // dialog can never wedge the queue.
   let chain = Promise.resolve();

   function enqueue(fn) {
      const run = chain.then(fn, fn);
      chain = run.then(
         function () {},
         function () {}
      );
      return run;
   }

   /**
    * @param {string} message
    * @param {object} [options] - {title, okText, cancelText, danger, detail}
    * @returns {Promise<boolean>} true if confirmed, false otherwise
    */
   function confirm(message, options) {
      const opts = Object.assign({}, options);
      return enqueue(function () {
         return new Promise(function (resolve) {
            opts.onClose = function (confirmed) {
               resolve(!!confirmed);
            };
            DawnSettingsModals.showConfirmModal(message, null, opts);
         });
      });
   }

   /**
    * @param {string} message
    * @param {object} [options] - {title, okText}
    * @returns {Promise<void>} resolves when dismissed
    */
   function alert(message, options) {
      const opts = Object.assign({}, options);
      if (!opts.title) opts.title = 'Notice';
      opts.okOnly = true;
      return enqueue(function () {
         return new Promise(function (resolve) {
            opts.onClose = function () {
               resolve();
            };
            DawnSettingsModals.showConfirmModal(message, null, opts);
         });
      });
   }

   /**
    * @param {string} message
    * @param {string} [defaultValue]
    * @param {object} [options] - {title, okText, cancelText, placeholder}
    * @returns {Promise<string|null>} entered string, or null if cancelled
    */
   function prompt(message, defaultValue, options) {
      const opts = Object.assign({}, options);
      return enqueue(function () {
         return new Promise(function (resolve) {
            opts.onClose = function (value) {
               resolve(value);
            };
            DawnSettingsModals.showInputModal(message, defaultValue || '', null, opts);
         });
      });
   }

   global.DawnDialog = {
      confirm: confirm,
      prompt: prompt,
      alert: alert,
   };
})(window);
