/**
 * DAWN WebUI - Unified Attachment Module
 * Routes files from a single attach button to vision (images) or documents
 * (everything else). Manages unified counter and drag-and-drop overlay.
 */
(function (global) {
   'use strict';

   // Allowed image MIME types (must match vision.js VALID_TYPES)
   const IMAGE_TYPES = new Set(['image/jpeg', 'image/png', 'image/gif', 'image/webp']);

   // DOM element references
   let attachBtn = null;
   let attachInput = null;
   let attachCounter = null;
   let dropdownBtn = null;
   let dropdownMenu = null;
   let attachEscToken = null; // DawnEscStack registration while the menu is open
   let dropTarget = null;

   /**
    * Initialize the attach module
    */
   function init() {
      attachBtn = document.getElementById('attach-btn');
      attachInput = document.getElementById('attach-file-input');
      attachCounter = document.getElementById('attach-counter');
      dropdownBtn = document.getElementById('attach-dropdown-btn');
      dropdownMenu = document.getElementById('attach-dropdown');
      dropTarget = document.getElementById('input-area');

      if (!attachBtn || !attachInput) {
         console.warn('Attach: Required elements not found');
         return;
      }

      // Attach button opens file picker
      attachBtn.addEventListener('click', () => attachInput.click());

      // File input change — route by type
      attachInput.addEventListener('change', handleFileSelect);

      // Chevron dropdown for camera
      bindDropdownEvents();

      // Unified drag-and-drop
      if (dropTarget) {
         dropTarget.addEventListener('dragenter', handleDragEnter);
         dropTarget.addEventListener('dragover', handleDragOver);
         dropTarget.addEventListener('dragleave', handleDragLeave);
         dropTarget.addEventListener('drop', handleDrop);
      }
   }

   // --- Dropdown (chevron split-button) ---

   function bindDropdownEvents() {
      if (!dropdownBtn || !dropdownMenu) return;

      dropdownBtn.addEventListener('click', toggleDropdown);

      // Dropdown item actions
      dropdownMenu.addEventListener('click', (e) => {
         const item = e.target.closest('[data-action]');
         if (!item) return;

         closeDropdown();

         if (item.dataset.action === 'camera' && typeof DawnVision !== 'undefined') {
            DawnVision.openCamera();
         }
      });

      // Close on outside click
      document.addEventListener('click', (e) => {
         if (!dropdownBtn.contains(e.target) && !dropdownMenu.contains(e.target)) {
            closeDropdown();
         }
      });

      // Keyboard: Escape closes, arrow keys navigate items
      dropdownBtn.addEventListener('keydown', (e) => {
         if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            openDropdown();
            const firstItem = dropdownMenu.querySelector('[role="menuitem"]');
            if (firstItem) firstItem.focus();
         }
      });

      // Escape close is handled via DawnEscStack (register-on-open in
      // openDropdown / unregister in closeDropdown); this keeps only the
      // arrow-key roving navigation.
      dropdownMenu.addEventListener('keydown', (e) => {
         if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            const items = Array.from(dropdownMenu.querySelectorAll('[role="menuitem"]'));
            const idx = items.indexOf(document.activeElement);
            const next = e.key === 'ArrowDown' ? idx + 1 : idx - 1;
            if (items[next]) items[next].focus();
         }
      });
   }

   function toggleDropdown() {
      if (dropdownMenu.classList.contains('hidden')) {
         openDropdown();
      } else {
         closeDropdown();
      }
   }

   function openDropdown() {
      if (!dropdownMenu || !dropdownBtn) return;
      dropdownMenu.classList.remove('hidden');
      dropdownBtn.setAttribute('aria-expanded', 'true');
      if (attachEscToken === null) {
         attachEscToken = DawnEscStack.register(() => {
            closeDropdown();
            dropdownBtn.focus();
            return true;
         });
      }
   }

   function closeDropdown() {
      if (!dropdownMenu || !dropdownBtn) return;
      dropdownMenu.classList.add('hidden');
      dropdownBtn.setAttribute('aria-expanded', 'false');
      if (attachEscToken !== null) {
         DawnEscStack.unregister(attachEscToken);
         attachEscToken = null;
      }
   }

   /**
    * Handle file input selection — route images and documents
    */
   function handleFileSelect(event) {
      const files = Array.from(event.target.files || []);
      if (files.length > 0) {
         routeFiles(files);
      }
      // Reset so same file can be selected again
      event.target.value = '';
   }

   /**
    * Route files to the appropriate pipeline
    */
   function routeFiles(files) {
      const imageFiles = [];
      const docFiles = [];

      for (const file of files) {
         if (IMAGE_TYPES.has(file.type)) {
            imageFiles.push(file);
         } else {
            docFiles.push(file);
         }
      }

      // Check unified slot limit
      const total = getTotalCount();
      const max = getMaxAttachments();
      const available = max - total;

      if (available <= 0) {
         DawnToast.show(`Maximum ${max} attachments reached`, 'warning');
         return;
      }

      // Apply per-type image sub-limit and vision check FIRST
      let imgSlots = imageFiles.length;
      let docSlots = docFiles.length;

      if (imgSlots > 0 && !DawnState.visionState.visionEnabled) {
         DawnToast.show('Vision not available for current model', 'warning');
         imgSlots = 0;
      }

      if (imgSlots > 0) {
         const imageMax = DawnState.visionState.maxImages;
         const imageCount = DawnState.visionState.pendingImages.length;
         const imageAvailable = imageMax - imageCount;
         if (imgSlots > imageAvailable) {
            if (imageAvailable <= 0) {
               DawnToast.show(`Maximum ${imageMax} images reached`, 'warning');
            } else {
               DawnToast.show(
                  `Only adding ${imageAvailable} image(s), image limit is ${imageMax}`,
                  'warning'
               );
            }
            imgSlots = Math.max(0, imageAvailable);
         }
      }

      // Then apply unified slot limit across remaining files
      if (imgSlots + docSlots > available) {
         const ratio = available / (imgSlots + docSlots);
         imgSlots = Math.min(imgSlots, Math.ceil(imgSlots * ratio));
         docSlots = Math.min(docSlots, available - imgSlots);
         DawnToast.show(`Only adding ${available} file(s), limit is ${max}`, 'warning');
      }

      // Process images
      if (imgSlots > 0 && typeof DawnVision !== 'undefined') {
         const toProcess = imageFiles.slice(0, imgSlots);
         toProcess.sort((a, b) => a.name.localeCompare(b.name));
         DawnVision.processMultipleImages(toProcess);
      }

      // Process documents
      if (docSlots > 0 && typeof DawnDocuments !== 'undefined') {
         DawnDocuments.processMultipleDocuments(docFiles.slice(0, docSlots));
      }
   }

   // --- Drag-and-drop (unified) ---

   function handleDragEnter(e) {
      e.preventDefault();
      const items = e.dataTransfer?.items;
      if (!items) return;
      const hasFiles = Array.from(items).some((item) => item.kind === 'file');
      if (hasFiles) {
         dropTarget.classList.add('drag-active');
      }
   }

   function handleDragOver(e) {
      e.preventDefault();
   }

   function handleDragLeave(e) {
      e.preventDefault();
      if (!dropTarget.contains(e.relatedTarget)) {
         dropTarget.classList.remove('drag-active');
      }
   }

   function handleDrop(e) {
      e.preventDefault();
      dropTarget.classList.remove('drag-active');

      const files = Array.from(e.dataTransfer?.files || []);
      if (files.length === 0) return;

      // Warn about unsupported non-image files
      const unsupported = files.filter(
         (f) =>
            !IMAGE_TYPES.has(f.type) &&
            typeof DawnDocuments !== 'undefined' &&
            !DawnDocuments.isAllowedExtension(f.name)
      );
      if (unsupported.length > 0) {
         const names = unsupported.map((f) => f.name).join(', ');
         DawnToast.show(`Unsupported file type: ${names}`, 'warning');
      }

      // Filter to supported files only
      const supported = files.filter(
         (f) =>
            IMAGE_TYPES.has(f.type) ||
            (typeof DawnDocuments !== 'undefined' && DawnDocuments.isAllowedExtension(f.name))
      );
      if (supported.length > 0) {
         routeFiles(supported);
      }
   }

   // --- Counter ---

   function getTotalCount() {
      return (
         DawnState.visionState.pendingImages.length +
         DawnState.documentState.pendingDocuments.length
      );
   }

   function getMaxAttachments() {
      return DawnState.attachmentState?.maxAttachments || 8;
   }

   /**
    * Update the unified attachment counter badge
    * Called by both DawnVision and DawnDocuments after add/remove
    */
   function updateCounter() {
      const count = getTotalCount();
      const max = getMaxAttachments();

      if (attachCounter) {
         if (count > 0) {
            attachCounter.textContent = `${count}/${max}`;
            attachCounter.setAttribute('aria-label', `${count} of ${max} attachments`);
            attachCounter.classList.remove('hidden');
         } else {
            attachCounter.classList.add('hidden');
         }
      }

      if (attachBtn) {
         const atLimit = count >= max;
         attachBtn.classList.toggle('at-limit', atLimit);
         attachBtn.title = atLimit
            ? `Maximum ${max} attachments reached`
            : 'Attach files or images';
      }
   }

   // Export module
   global.DawnAttach = {
      init,
      updateCounter,
      getTotalCount,
      getMaxAttachments,
   };
})(window);
