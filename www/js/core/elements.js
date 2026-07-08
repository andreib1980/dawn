/**
 * DAWN DOM Elements Cache Module
 * Centralized DOM element references to avoid repeated queries
 *
 * Usage:
 *   DawnElements.init()              // Call after DOM ready
 *   DawnElements.transcript          // Access cached element
 */
(function (global) {
   'use strict';

   // Cached element references (populated by init)
   const elements = {
      // Connection status
      connectionStatus: null,

      // Ring visualization
      ringContainer: null,
      ringSvg: null,
      ringFft: null,
      ringThroughput: null,
      ringHesitation: null,
      ringInner: null,

      // Waveform paths
      waveformPath: null,
      waveformTrails: [],

      // Status display
      statusDot: null,
      statusText: null,

      // Transcript
      transcript: null,

      // Input
      textInput: null,
      actionBtn: null,
      actionDropdownBtn: null,
      debugBtn: null,

      // Visualizer collapse
      visualizer: null,
      visualizerMini: null,
      miniStatusDot: null,
      miniStatusText: null,
      miniContextValue: null,
      composerBusy: null,
      composerBusyLabel: null,
      miniContext: null,
      visualizerCollapseToggle: null,
   };

   /**
    * Initialize all DOM element references
    * Call this after DOMContentLoaded
    */
   function init() {
      // Connection status
      elements.connectionStatus = document.getElementById('connection-status');

      // Ring visualization
      elements.ringContainer = document.getElementById('ring-container');
      elements.ringSvg = document.getElementById('ring-svg');
      elements.ringFft = document.getElementById('ring-fft');
      elements.ringThroughput = document.getElementById('ring-throughput');
      elements.ringHesitation = document.getElementById('ring-hesitation');
      elements.ringInner = document.getElementById('ring-inner');

      // Waveform paths
      elements.waveformPath = document.getElementById('waveform-path');
      elements.waveformTrails = [
         document.getElementById('waveform-trail-1'),
         document.getElementById('waveform-trail-2'),
         document.getElementById('waveform-trail-3'),
         document.getElementById('waveform-trail-4'),
         document.getElementById('waveform-trail-5'),
      ];

      // Status display
      elements.statusDot = document.getElementById('status-dot');
      elements.statusText = document.getElementById('status-text');

      // Transcript
      elements.transcript = document.getElementById('transcript');

      // Input
      elements.textInput = document.getElementById('text-input');
      elements.actionBtn = document.getElementById('action-btn');
      elements.actionDropdownBtn = document.getElementById('action-dropdown-btn');
      elements.debugBtn = document.getElementById('debug-btn');

      // Visualizer collapse
      elements.visualizer = document.getElementById('visualizer');
      elements.visualizerMini = document.getElementById('visualizer-mini');
      elements.miniStatusDot = document.getElementById('mini-status-dot');
      elements.miniStatusText = document.getElementById('mini-status-text');
      elements.composerBusy = document.getElementById('composer-busy');
      elements.composerBusyLabel = document.getElementById('composer-busy-label');
      elements.miniContextValue = document.getElementById('mini-context-value');
      elements.miniContext = document.querySelector('.mini-context');
      elements.visualizerCollapseToggle = document.getElementById('visualizer-collapse');
   }

   // Expose globally - elements object with init method
   global.DawnElements = Object.assign(elements, {
      init: init,
   });
})(window);
