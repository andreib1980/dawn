/**
 * DAWN Theme Module
 * Color theme switching with localStorage persistence
 *
 * Usage:
 *   DawnTheme.set('purple')     // Set theme
 *   DawnTheme.init()            // Initialize from localStorage + bind buttons
 *   DawnTheme.current()         // Get current theme name
 *   DawnTheme.THEMES            // Array of theme names
 *   DawnTheme.COLORS            // Theme name -> hex color map
 */
(function (global) {
   'use strict';

   const THEMES = ['cyan', 'purple', 'green', 'orange', 'red', 'blue', 'terminal'];

   // KEEP IN SYNC: the pre-paint theme bootstrap COLORS maps in index.html and
   // login.html (<head>) + js/core/storage.js KEYS.THEME.  Add/rename a theme →
   // update all four.
   const THEME_COLORS = {
      cyan: '#2dd4bf',
      purple: '#a855f7',
      green: '#22c55e',
      orange: '#f97316',
      red: '#f87171',
      blue: '#3b82f6',
      terminal: '#7fff7f',
   };

   // Cached for performance (avoids DOM query per FFT frame)
   let currentTheme = 'cyan';

   /**
    * Set the active color theme
    * @param {string} theme - Theme name from THEMES array
    */
   function setTheme(theme) {
      if (!THEMES.includes(theme)) theme = 'cyan';

      // Cache for FFT performance
      currentTheme = theme;

      // Apply theme to document
      if (theme === 'cyan') {
         document.documentElement.removeAttribute('data-theme');
      } else {
         document.documentElement.setAttribute('data-theme', theme);
      }

      // Persist to localStorage
      DawnStore.set(DawnStore.KEYS.THEME, theme);

      // Update active button
      document.querySelectorAll('.theme-btn').forEach((btn) => {
         btn.classList.toggle('active', btn.dataset.theme === theme);
      });

      // Update meta theme-color for mobile browsers
      const metaTheme = document.querySelector('meta[name="theme-color"]');
      if (metaTheme) {
         metaTheme.setAttribute('content', THEME_COLORS[theme] || THEME_COLORS.cyan);
      }
   }

   /**
    * Initialize theme from localStorage and bind button handlers
    */
   function initTheme() {
      const saved = DawnStore.get(DawnStore.KEYS.THEME, null);
      setTheme(saved || 'cyan');

      // Add click handlers to theme buttons
      document.querySelectorAll('.theme-btn').forEach((btn) => {
         btn.addEventListener('click', () => setTheme(btn.dataset.theme));
      });
   }

   /**
    * Get current theme name
    * @returns {string} Current theme
    */
   function getCurrentTheme() {
      return currentTheme;
   }

   // Expose globally
   global.DawnTheme = {
      THEMES: THEMES,
      COLORS: THEME_COLORS,
      set: setTheme,
      init: initTheme,
      current: getCurrentTheme,
   };
})(window);
