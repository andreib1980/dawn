/**
 * DAWN Settings - Search
 * Provides live filtering and highlighting of settings fields
 */
(function () {
   'use strict';

   // DOM references
   let searchInput = null;
   let clearBtn = null;
   let sectionsContainer = null;
   let emptyState = null;

   // Search index: [{sectionEl, fieldEl|null, searchText, labelEl}]
   let index = [];

   // Collapsed state snapshot (saved when search starts)
   let collapsedSnapshot = null;

   /**
    * Initialize search module — cache DOM refs
    */
   function init() {
      searchInput = document.getElementById('settings-search-input');
      clearBtn = document.getElementById('settings-search-clear');
      sectionsContainer = document.getElementById('settings-sections');
      emptyState = document.getElementById('settings-search-empty');

      if (searchInput) {
         const Utils = window.DawnSettingsUtils;
         const debouncedInput = Utils.debounce(handleInput, 120);
         searchInput.addEventListener('input', debouncedInput);
      }
      if (clearBtn) {
         clearBtn.addEventListener('click', clearSearch);
      }
   }

   /**
    * Build search index from rendered settings sections
    * Call after renderSettingsSections() completes
    */
   function buildIndex() {
      index = [];
      if (!sectionsContainer) return;

      const sections = sectionsContainer.querySelectorAll('.settings-section');
      sections.forEach((sectionEl) => {
         // Index section header
         const header = sectionEl.querySelector('.section-header');
         if (header) {
            // Get just the text label (skip icon and toggle spans)
            const labelText = getHeaderLabelText(header);
            index.push({
               sectionEl: sectionEl,
               fieldEl: null,
               searchText: labelText.toLowerCase(),
               labelEl: header,
               originalText: labelText,
            });
         }

         // Index individual fields
         const fields = sectionEl.querySelectorAll('.setting-item, .setting-item-row');
         fields.forEach((fieldEl) => {
            const label = fieldEl.querySelector('label');
            if (label) {
               const text = label.textContent.trim();
               index.push({
                  sectionEl: sectionEl,
                  fieldEl: fieldEl,
                  searchText: text.toLowerCase(),
                  labelEl: label,
                  originalText: text,
               });
            }
         });

         // Index group labels
         const groups = sectionEl.querySelectorAll('.setting-group .group-label');
         groups.forEach((groupLabel) => {
            const text = groupLabel.textContent.trim();
            index.push({
               sectionEl: sectionEl,
               fieldEl: groupLabel.closest('.setting-group'),
               searchText: text.toLowerCase(),
               labelEl: groupLabel,
               originalText: text,
            });
         });
      });

      // If search input has an active query, re-apply filtering
      if (searchInput && searchInput.value.trim().length >= 2) {
         collapsedSnapshot = null;
         handleInput();
      }
   }

   /**
    * Extract the text label from a section header (ignoring icon and toggle spans)
    */
   function getHeaderLabelText(header) {
      let text = '';
      header.childNodes.forEach((node) => {
         if (node.nodeType === Node.TEXT_NODE) {
            text += node.textContent;
         }
      });
      return text.trim();
   }

   /**
    * Handle search input
    */
   function handleInput() {
      if (!searchInput) return;

      const query = searchInput.value.trim();

      // Show/hide clear button
      if (clearBtn) {
         clearBtn.classList.toggle('hidden', query.length === 0);
      }

      // Require at least 2 characters
      if (query.length < 2) {
         if (collapsedSnapshot) {
            restoreState();
         }
         return;
      }

      // Snapshot collapsed state on first qualifying query
      if (!collapsedSnapshot) {
         snapshotCollapsedState();
      }

      const q = query.toLowerCase();
      const matchedSections = new Set();

      // Temporarily show advanced fields so search can find them
      if (sectionsContainer) {
         sectionsContainer.classList.add('show-advanced');
      }

      // Walk index and toggle visibility
      index.forEach((entry) => {
         const matches = entry.searchText.includes(q);

         if (matches) {
            matchedSections.add(entry.sectionEl);
         }

         // Restore original text first (remove any previous <mark> highlights)
         restoreLabel(entry);

         if (entry.fieldEl) {
            // Field-level entry
            entry.fieldEl.classList.toggle('search-hidden', !matches);
            if (matches) {
               highlightLabel(entry, q);
            }
         } else {
            // Section-level entry — highlighting handled below
            if (matches) {
               highlightLabel(entry, q);
            }
         }
      });

      // Post-pass: a setting-group's label rarely matches a query about a
      // field inside it, so the group's own index entry would re-add
      // search-hidden and hide its visible children.  Un-hide any group that
      // still contains a visible field.
      if (sectionsContainer) {
         sectionsContainer.querySelectorAll('.setting-group').forEach((group) => {
            const hasVisibleField = group.querySelector(
               '.setting-item:not(.search-hidden), .setting-item-row:not(.search-hidden)'
            );
            if (hasVisibleField) {
               group.classList.remove('search-hidden');
            }
         });
      }

      // Show/hide sections based on whether they have matches
      if (sectionsContainer) {
         const sections = sectionsContainer.querySelectorAll('.settings-section');
         sections.forEach((sectionEl) => {
            if (matchedSections.has(sectionEl)) {
               sectionEl.classList.remove('search-hidden', 'collapsed');
            } else {
               sectionEl.classList.add('search-hidden');
            }
         });

         // Show/hide category headers based on whether their sections have matches
         const categoryHeaders = sectionsContainer.querySelectorAll('.settings-category-header');
         categoryHeaders.forEach((headerEl) => {
            const categoryId = headerEl.dataset.category;
            const categorySections = sectionsContainer.querySelectorAll(
               '.settings-section[data-category="' + categoryId + '"]'
            );
            let hasVisible = false;
            categorySections.forEach((s) => {
               if (!s.classList.contains('search-hidden')) hasVisible = true;
            });
            headerEl.classList.toggle('search-hidden', !hasVisible);
         });
      }

      // Toggle empty state
      if (emptyState) {
         emptyState.classList.toggle('hidden', matchedSections.size > 0);
      }
   }

   /**
    * Save collapsed state of all sections
    */
   function snapshotCollapsedState() {
      collapsedSnapshot = new Map();
      if (!sectionsContainer) return;

      const sections = sectionsContainer.querySelectorAll('.settings-section');
      sections.forEach((sectionEl) => {
         collapsedSnapshot.set(sectionEl, sectionEl.classList.contains('collapsed'));
      });
   }

   /**
    * Restore collapsed state and remove search classes
    */
   function restoreState() {
      if (!sectionsContainer) return;

      // Remove search-hidden from everything
      sectionsContainer.querySelectorAll('.search-hidden').forEach((el) => {
         el.classList.remove('search-hidden');
      });

      // Restore collapsed state
      if (collapsedSnapshot) {
         collapsedSnapshot.forEach((wasCollapsed, sectionEl) => {
            sectionEl.classList.toggle('collapsed', wasCollapsed);
         });
         collapsedSnapshot = null;
      }

      // Remove all highlights
      index.forEach((entry) => restoreLabel(entry));

      // Restore advanced toggle to its persisted state
      if (sectionsContainer) {
         const showAdvanced = localStorage.getItem('dawn-settings-show-advanced') === 'true';
         sectionsContainer.classList.toggle('show-advanced', showAdvanced);
      }

      // Hide empty state
      if (emptyState) {
         emptyState.classList.add('hidden');
      }
   }

   /**
    * Highlight matching substring in a label with <mark>
    */
   function highlightLabel(entry, query) {
      const el = entry.labelEl;
      const original = entry.originalText;
      const lowerOriginal = original.toLowerCase();
      const idx = lowerOriginal.indexOf(query);

      if (idx === -1) return;

      const before = original.substring(0, idx);
      const match = original.substring(idx, idx + query.length);
      const after = original.substring(idx + query.length);

      // For section headers, we need to preserve the icon and toggle spans
      if (!entry.fieldEl) {
         // Section header — only replace text nodes
         el.childNodes.forEach((node) => {
            if (node.nodeType === Node.TEXT_NODE && node.textContent.trim().length > 0) {
               const span = document.createElement('span');
               span.className = 'search-label-text';
               span.innerHTML =
                  escapeHtml(before) + '<mark>' + escapeHtml(match) + '</mark>' + escapeHtml(after);
               el.replaceChild(span, node);
            }
         });
      } else {
         // Field label — safe to replace innerHTML but preserve restart badge
         const badge = el.querySelector('.restart-badge');
         el.innerHTML =
            escapeHtml(before) + '<mark>' + escapeHtml(match) + '</mark>' + escapeHtml(after) + ' ';
         if (badge) el.appendChild(badge);
      }
   }

   /**
    * Restore original label text (remove highlights)
    */
   function restoreLabel(entry) {
      const el = entry.labelEl;

      if (!entry.fieldEl) {
         // Section header — replace any search-label-text spans back with text nodes
         const searchSpan = el.querySelector('.search-label-text');
         if (searchSpan) {
            const textNode = document.createTextNode(entry.originalText);
            el.replaceChild(textNode, searchSpan);
         }
      } else if (el.querySelector('mark')) {
         // Field label — restore text, preserve restart badge
         const badge = el.querySelector('.restart-badge');
         el.textContent = entry.originalText + ' ';
         if (badge) el.appendChild(badge);
      }
   }

   /**
    * Clear search input and restore normal view
    */
   function clearSearch() {
      if (searchInput) {
         searchInput.value = '';
      }
      if (clearBtn) {
         clearBtn.classList.add('hidden');
      }
      restoreState();
   }

   /**
    * Escape HTML for safe insertion
    */
   function escapeHtml(str) {
      if (typeof DawnFormat !== 'undefined') return DawnFormat.escapeHtml(str);
      const div = document.createElement('div');
      div.textContent = str;
      return div.innerHTML;
   }

   // Export
   window.DawnSettingsSearch = {
      init: init,
      buildIndex: buildIndex,
      clearSearch: clearSearch,
   };
})();
