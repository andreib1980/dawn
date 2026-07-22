/**
 * DAWN Escape / dismiss stack
 * A single global LIFO registry implementing "Escape closes the topmost layer,"
 * so overlays (modals, dropdowns, panels) no longer each wire their own
 * document-level Escape handler with fragile "is another layer open?" guards.
 *
 * A layer registers a handler when it opens and unregisters when it closes.  On
 * Escape the stack invokes handlers top-first; a handler returns `true` when it
 * consumed the key (stop), or `false` to fall through to the next layer down
 * (supports staged behavior, e.g. "clear the search box first, then close").
 *
 * EXCLUSIONS — element-scoped Escape stays element-scoped, NOT on this stack:
 *   - transient toasts (a background toast must never eat a modal's Escape)
 *   - persistent panels whose Escape is a focus-scoped shortcut (phone call panel)
 *   - WAI-ARIA menu roving-focus (Escape on a menu item returns focus to the button)
 * Only document-level "dismiss this layer" Escape belongs here.
 *
 * LIFECYCLE CONTRACT (this is the highest-risk surface — honor it):
 *   - unregister on EVERY exit path: Escape-consumed, outside-click, backdrop,
 *     programmatic close, and DOM re-render/teardown.  Register with a
 *     `token === null` (or unregister-before-register) guard so re-opening a
 *     surface can't orphan a prior registration.
 *   - a handler that throws (e.g. it closed over a now-detached node) is
 *     auto-unregistered so one broken layer can't wedge the whole stack.
 *   - CAVEAT: the stack is throw-safe but not no-op-safe — a stale handler that
 *     returns `true` without actually closing (e.g. a close() that no-ops on an
 *     already-hidden element) still CONSUMES the Escape and blocks lower layers.
 *     The unregister-on-every-path discipline is what prevents that.
 *
 * Long-term direction: the platform <dialog>.showModal() (top-layer + built-in
 * Escape via the `cancel` event + background inerting) and the Popover API
 * (light-dismiss) supersede this for NEW work.  This stack retrofits the
 * existing hidden-DOM overlays; don't let it accrete indefinitely.
 *
 * Usage:
 *   // in open():   if (token === null) token = DawnEscStack.register(() => { closeMe(); return true; });
 *   // in close():  if (token !== null) { DawnEscStack.unregister(token); token = null; }
 * A menu/popover keeps its own outside-click + arrow-key roving element-scoped;
 * only the Escape-to-dismiss goes on this stack.
 */
(function (global) {
   'use strict';

   // Stack of { token, handler }.  Top of stack = end of array = closed first.
   const stack = [];
   let nextToken = 1;

   /**
    * Register a dismiss handler.  Handler returns true to consume Escape, false
    * to fall through to the layer below.
    * @param {function(): boolean} handler
    * @returns {number} token for unregister()
    */
   function register(handler) {
      const token = nextToken++;
      stack.push({ token: token, handler: handler });
      return token;
   }

   /**
    * Remove a previously-registered handler.  Safe to call more than once.
    * @param {number} token
    */
   function unregister(token) {
      for (let i = stack.length - 1; i >= 0; i--) {
         if (stack[i].token === token) {
            stack.splice(i, 1);
            return;
         }
      }
   }

   // Walk top-first, calling handlers until one consumes (returns true).  A
   // handler that throws is dropped so a stale/broken layer can't wedge the
   // stack.  Returns true if some handler consumed the key.
   function dispatch() {
      for (let i = stack.length - 1; i >= 0; i--) {
         const entry = stack[i];
         let consumed = false;
         try {
            consumed = entry.handler() === true;
         } catch (e) {
            stack.splice(i, 1); // broken handler — auto-unregister and continue
            continue;
         }
         if (consumed) return true;
      }
      return false;
   }

   // Capture phase so the stack arbitrates before element-scoped handlers.
   document.addEventListener(
      'keydown',
      (e) => {
         if (e.key !== 'Escape') return;
         if (e.isComposing) return; // Escape is cancelling IME composition, not a layer
         if (stack.length === 0) return;
         if (dispatch()) {
            e.preventDefault();
            e.stopImmediatePropagation();
         }
      },
      true
   );

   global.DawnEscStack = {
      register: register,
      unregister: unregister,
   };
})(window);
