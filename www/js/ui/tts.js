/**
 * DAWN TTS Module
 * Text-to-speech toggle with localStorage persistence
 *
 * Usage:
 *   DawnTts.init()           // Initialize from localStorage + bind toggle button
 *   DawnTts.isEnabled()      // Check if TTS is enabled
 *   DawnTts.setEnabled(bool) // Set TTS state programmatically
 */
(function (global) {
   'use strict';

   // TTS state (persisted to localStorage)
   let ttsEnabled = DawnStore.getBool(DawnStore.KEYS.TTS_ENABLED, false);

   /**
    * Check if TTS is currently enabled
    * @returns {boolean}
    */
   function isEnabled() {
      return ttsEnabled;
   }

   /**
    * Set TTS enabled state
    * @param {boolean} enabled - Whether to enable TTS
    * @param {Object} options - Optional settings
    * @param {boolean} options.notifyServer - Send update to server (default: true)
    * @param {boolean} options.stopPlayback - Stop current audio when disabling (default: true)
    */
   function setEnabled(enabled, options = {}) {
      const { notifyServer = true, stopPlayback = true } = options;

      ttsEnabled = enabled;
      DawnStore.setBool(DawnStore.KEYS.TTS_ENABLED, ttsEnabled);
      updateUI(ttsEnabled);

      // Stop any currently playing audio when disabling
      if (!ttsEnabled && stopPlayback && typeof DawnAudioPlayback !== 'undefined') {
         DawnAudioPlayback.stop();
      }

      // Notify server
      if (notifyServer && typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'set_tts_enabled', payload: { enabled: ttsEnabled } });
      }

      console.log('TTS', ttsEnabled ? 'enabled' : 'disabled');
   }

   /**
    * Toggle TTS state
    */
   function toggle() {
      setEnabled(!ttsEnabled);
   }

   /**
    * Update UI elements to reflect TTS state
    * @param {boolean} enabled - Current TTS state
    */
   function updateUI(enabled) {
      const btn = document.getElementById('tts-toggle');
      const iconOn = document.getElementById('tts-icon-on');
      const iconOff = document.getElementById('tts-icon-off');

      if (!btn || !iconOn || !iconOff) return;

      iconOn.classList.toggle('hidden', !enabled);
      iconOff.classList.toggle('hidden', enabled);
      btn.classList.toggle('active', enabled);

      // Dynamic ARIA updates for accessibility
      btn.setAttribute('aria-pressed', enabled);
      btn.setAttribute('aria-label', enabled ? 'Speech output enabled' : 'Speech output disabled');
      btn.setAttribute('title', enabled ? 'Disable speech output' : 'Enable speech output');
   }

   /**
    * Initialize TTS module
    * Sets up toggle button and syncs UI with current state
    */
   function init() {
      const btn = document.getElementById('tts-toggle');
      if (!btn) return;

      // Initialize visual state from current ttsEnabled
      updateUI(ttsEnabled);

      // Note: TTS state is synced to server via the WebSocket init/reconnect handshake
      // (websocket.js sends tts_enabled in both 'init' and 'reconnect' messages)

      // Bind click handler
      btn.addEventListener('click', toggle);
   }

   // Expose globally
   global.DawnTts = {
      init: init,
      isEnabled: isEnabled,
      setEnabled: setEnabled,
      toggle: toggle,
   };
})(window);
