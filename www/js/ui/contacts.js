/**
 * DAWN Contacts Panel Module
 * Manages contact viewing, searching, adding, editing, and deletion
 * within the Memory popover's Contacts tab.
 */
(function () {
   'use strict';

   const PAGE_SIZE = 20;

   /* =============================================================================
    * State
    * ============================================================================= */

   let state = {
      contacts: [],
      searchQuery: '',
      searchTimeout: null,
      loading: false,
      offset: 0,
      hasMore: false,
      filterEntity: null, // {id, name} when navigating from Graph tab
      editingContact: null, // contact object being edited, or null for add mode
      pendingAction: null, // 'add' or 'update' - tracks what we're waiting for
      // Typeahead state for entity name input
      selectedEntityId: null, // If user selected an existing entity
      typeaheadTimeout: null,
      typeaheadResults: [], // Latest entity search results
      forceCreate: false, // Skip disambiguation on next add
   };

   /* =============================================================================
    * SVG Icons
    * ============================================================================= */

   const ICON_EDIT =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
      '<path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>' +
      '<path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/></svg>';

   const ICON_TRASH =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
      '<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>';

   const ICON_PLUS =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
      '<line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>';

   const ICON_CONTACT =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
      '<path d="M16 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/>' +
      '<circle cx="8.5" cy="7" r="4"/>' +
      '<line x1="20" y1="8" x2="20" y2="14"/><line x1="23" y1="11" x2="17" y2="11"/></svg>';

   const ICON_PERSON_SMALL =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">' +
      '<circle cx="12" cy="8" r="5"/>' +
      '<path d="M3 21c0-4.97 4.03-9 9-9s9 4.03 9 9"/></svg>';

   const PHOTO_MAX_DIM = 256;

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestList(offset) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      state.loading = true;
      DawnWS.send({
         type: 'contacts_list',
         payload: { limit: PAGE_SIZE, offset: offset || 0 },
      });
   }

   function requestSearch(query) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      if (!query || query.trim().length === 0) return;
      state.loading = true;
      DawnWS.send({
         type: 'contacts_search',
         payload: { name: query.trim() },
      });
   }

   function requestAdd(entityName, entityId, fieldType, value, label, forceCreate) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      state.pendingAction = 'add';
      const payload = { field_type: fieldType, value: value, label: label };
      if (entityId) {
         payload.entity_id = entityId;
      } else {
         payload.entity_name = entityName;
         if (forceCreate) payload.force_create = true;
      }
      DawnWS.send({ type: 'contacts_add', payload: payload });
   }

   function requestUpdate(contactId, fieldType, value, label) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      state.pendingAction = 'update';
      DawnWS.send({
         type: 'contacts_update',
         payload: { contact_id: contactId, field_type: fieldType, value: value, label: label },
      });
   }

   function requestDelete(contactId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'contacts_delete',
         payload: { contact_id: contactId },
      });
   }

   function requestSearchEntities(query) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'contacts_search_entities',
         payload: { query: query },
      });
   }

   function requestSetPhoto(entityId, photoId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_set_photo',
         payload: { entity_id: entityId, photo_id: photoId || null },
      });
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      state.loading = false;

      if (!payload.success) {
         console.error('Failed to list contacts:', payload.error);
         return;
      }

      const newContacts = payload.contacts || [];
      state.hasMore = payload.has_more || false;

      if (state.offset > 0 && newContacts.length > 0) {
         state.contacts = state.contacts.concat(newContacts);
         appendContactCards(newContacts);
      } else {
         state.contacts = newContacts;
         renderContactList();
      }

      updateLoadMoreButton();
   }

   function handleSearchResponse(payload) {
      state.loading = false;

      if (!payload.success) {
         console.error('Failed to search contacts:', payload.error);
         return;
      }

      state.contacts = payload.contacts || [];
      state.hasMore = false;
      renderContactList();
      updateLoadMoreButton();
   }

   function handleAddResponse(payload) {
      state.pendingAction = null;
      setSaveButtonEnabled(true);
      if (!payload.success) {
         if (payload.needs_disambiguation && payload.candidates) {
            showDisambiguation(payload.candidates, payload.entered_name);
            return;
         }
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Failed to add contact: ' + (payload.error || ''), 'error');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Contact saved', 'success');

      // If we have a pending photo and the server returned an entity_id, set it now
      if (state.pendingPhotoId && payload.entity_id) {
         requestSetPhoto(payload.entity_id, state.pendingPhotoId);
         state.pendingPhotoId = null;
      }

      // Keep modal open for rapid entry — clear value but keep name/type
      const valueInput = document.getElementById('contact-value');
      if (valueInput) {
         valueInput.value = '';
         valueInput.focus();
      }
      flashSaveButton();
      reload();
   }

   function handleUpdateResponse(payload) {
      state.pendingAction = null;
      setSaveButtonEnabled(true);
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Failed to update contact: ' + (payload.error || ''), 'error');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Contact updated', 'success');
      closeModal();
      reload();
   }

   function handleDeleteResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') DawnToast.show('Failed to delete contact', 'error');
         return;
      }
      if (typeof DawnToast !== 'undefined') DawnToast.show('Contact deleted', 'success');
      reload();
   }

   function handleSearchEntitiesResponse(payload) {
      if (!payload.success) return;
      state.typeaheadResults = payload.entities || [];
      renderTypeaheadDropdown();
   }

   function handleEnsureEntityResponse(payload) {
      if (state.pendingAction !== 'photo_only') return;
      state.pendingAction = null;
      setSaveButtonEnabled(true);

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Failed to create entity: ' + (payload.error || ''), 'error');
         return;
      }

      if (state.pendingPhotoId && payload.entity_id) {
         requestSetPhoto(payload.entity_id, state.pendingPhotoId);
         state.pendingPhotoId = null;
         state.selectedEntityId = payload.entity_id;
         /* Don't flash/reload yet — wait for handleSetPhotoResponse */
      }
   }

   function handleSetPhotoResponse(payload) {
      const wasPhotoOnly = state.pendingAction === 'photo_only';
      const wasRemove = state.pendingAction === 'photo_remove';
      if (wasPhotoOnly || wasRemove) {
         state.pendingAction = null;
         setSaveButtonEnabled(true);
      }

      if (!payload.success) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Failed to set photo: ' + (payload.error || ''), 'error');
         return;
      }

      if (wasRemove) {
         resetPhotoUI();
         if (state.editingContact) {
            state.editingContact.photo_id = null;
         }
      }
      if (wasPhotoOnly) {
         flashSaveButton();
      }
      reload();
   }

   /* =============================================================================
    * Rendering
    * ============================================================================= */

   function getListContainer() {
      return document.getElementById('memory-list');
   }

   function renderContactList() {
      const container = getListContainer();
      if (!container) return;

      let html = '';

      // Filter banner
      if (state.filterEntity) {
         html +=
            '<div class="contact-filter-banner">' +
            'Showing contacts for <strong>' +
            escapeHtml(state.filterEntity.name) +
            '</strong>' +
            '<button class="contact-filter-clear" data-action="clear-filter">Show all</button>' +
            '</div>';
      }

      // Add button
      html +=
         '<div class="contact-add-row">' +
         '<button class="contact-add-btn" data-action="add-contact">' +
         ICON_PLUS +
         ' Add Contact</button></div>';

      if (state.contacts.length === 0) {
         html += '<div class="memory-empty">No contacts found</div>';
      } else {
         html += state.contacts.map((c) => renderContactCard(c)).join('');
      }

      container.innerHTML = html;
   }

   function appendContactCards(contacts) {
      const container = getListContainer();
      if (!container || contacts.length === 0) return;

      const html = contacts.map((c) => renderContactCard(c)).join('');
      container.insertAdjacentHTML('beforeend', html);
   }

   function renderPhotoThumb(contact) {
      if (contact.photo_id) {
         return (
            '<img class="contact-photo-thumb" src="/api/images/' +
            encodeURIComponent(contact.photo_id) +
            '" alt="" loading="lazy">'
         );
      }
      return '<span class="contact-photo-thumb-placeholder">' + ICON_PERSON_SMALL + '</span>';
   }

   function handleThumbError(e) {
      if (e.target.classList.contains('contact-photo-thumb')) {
         const span = document.createElement('span');
         span.className = 'contact-photo-thumb-placeholder';
         span.innerHTML = ICON_PERSON_SMALL;
         e.target.replaceWith(span);
      }
   }

   function renderContactCard(contact) {
      return (
         '<div class="contact-card" data-contact-id="' +
         contact.contact_id +
         '">' +
         // Row 1: Photo thumb, Name, label badge, action buttons
         '<div class="contact-card-header">' +
         renderPhotoThumb(contact) +
         '<span class="contact-entity-name">' +
         escapeHtml(contact.entity_name) +
         '</span>' +
         (contact.label
            ? '<span class="dawn-badge muted contact-field-label">' +
              escapeHtml(contact.label) +
              '</span>'
            : '') +
         '<div class="contact-actions">' +
         '<button class="contact-edit-btn" data-action="edit" data-contact-id="' +
         contact.contact_id +
         '" title="Edit">' +
         ICON_EDIT +
         '</button>' +
         '<button class="contact-delete-btn" data-action="delete" data-contact-id="' +
         contact.contact_id +
         '" title="Delete">' +
         ICON_TRASH +
         '</button>' +
         '</div>' +
         '</div>' +
         // Row 2: Type badge, value
         '<div class="contact-card-row">' +
         '<span class="dawn-badge accent contact-field-type">' +
         escapeHtml(contact.field_type) +
         '</span>' +
         '<span class="contact-field-value">' +
         escapeHtml(contact.value) +
         '</span>' +
         '</div>' +
         '</div>'
      );
   }

   function updateLoadMoreButton() {
      const btn = document.getElementById('memory-load-more');
      if (!btn) return;
      if (state.hasMore && !state.searchQuery) {
         btn.classList.remove('hidden');
         btn.disabled = false;
      } else {
         btn.classList.add('hidden');
      }
   }

   /* =============================================================================
    * Modal
    * ============================================================================= */

   function openModal(contact) {
      state.editingContact = contact || null;
      state.selectedEntityId = null;
      state.typeaheadResults = [];
      state.pendingPhotoId = null;

      const modal = document.getElementById('contact-modal');
      const title = document.getElementById('contact-modal-title');
      const nameInput = document.getElementById('contact-entity-name');
      const typeSelect = document.getElementById('contact-field-type');
      const valueInput = document.getElementById('contact-value');
      const labelSelect = document.getElementById('contact-label');
      const hint = document.getElementById('contact-entity-hint');

      if (!modal) return;

      // Clear any existing typeahead dropdown
      closeTypeaheadDropdown();

      // Reset photo UI and show existing photo if available
      resetPhotoUI();
      if (contact && contact.photo_id) {
         const preview = document.getElementById('contact-photo-preview');
         const placeholder = document.getElementById('contact-photo-placeholder');
         if (preview) {
            preview.src = '/api/images/' + encodeURIComponent(contact.photo_id);
            preview.classList.remove('hidden');
            preview.onerror = () => {
               preview.classList.add('hidden');
               if (placeholder) placeholder.classList.remove('hidden');
               showRemoveButton(false);
            };
         }
         if (placeholder) placeholder.classList.add('hidden');
         showRemoveButton(true);
      }

      if (contact) {
         title.textContent = 'Edit Contact';
         nameInput.value = contact.entity_name || '';
         nameInput.disabled = true;
         typeSelect.value = contact.field_type || 'email';
         valueInput.value = contact.value || '';
         labelSelect.value = contact.label || '';
         if (hint) hint.classList.add('hidden');
      } else {
         title.textContent = 'Add Contact';
         if (state.filterEntity) {
            nameInput.value = state.filterEntity.name;
            state.selectedEntityId = state.filterEntity.id;
            if (hint) {
               hint.textContent = 'Using existing entity: ' + state.filterEntity.name;
               hint.className = 'contact-entity-hint';
            }
         } else {
            nameInput.value = '';
            if (hint) hint.classList.add('hidden');
         }
         nameInput.disabled = false;
         typeSelect.value = 'email';
         valueInput.value = '';
         labelSelect.value = '';
      }

      updateValueInputType(typeSelect.value);
      modal.classList.remove('hidden');
      document.addEventListener('keydown', handleModalKeydown);
      if (!contact) {
         nameInput.focus();
      } else {
         valueInput.focus();
      }
   }

   function handleModalKeydown(e) {
      if (e.key === 'Escape') {
         closeModal();
         return;
      }
      if (e.key === 'Tab') {
         const modal = document.getElementById('contact-modal');
         if (!modal) return;
         const focusable = [
            ...modal.querySelectorAll(
               'input:not(.hidden), select, button:not(.hidden), [tabindex]:not([tabindex="-1"])'
            ),
         ].filter((el) => el.offsetParent !== null && !el.disabled);
         if (focusable.length === 0) return;
         const first = focusable[0];
         const last = focusable[focusable.length - 1];
         if (e.shiftKey && document.activeElement === first) {
            e.preventDefault();
            last.focus();
         } else if (!e.shiftKey && document.activeElement === last) {
            e.preventDefault();
            first.focus();
         }
      }
   }

   function closeModal() {
      const modal = document.getElementById('contact-modal');
      if (modal) modal.classList.add('hidden');
      document.removeEventListener('keydown', handleModalKeydown);
      state.editingContact = null;
      state.selectedEntityId = null;
      state.typeaheadResults = [];
      setSaveButtonEnabled(true);
      closeTypeaheadDropdown();
   }

   function handleModalSave() {
      const nameInput = document.getElementById('contact-entity-name');
      const typeSelect = document.getElementById('contact-field-type');
      const valueInput = document.getElementById('contact-value');
      const labelSelect = document.getElementById('contact-label');

      const entityName = (nameInput.value || '').trim();
      const fieldType = typeSelect.value;
      const value = (valueInput.value || '').trim();
      const label = labelSelect.value;

      if (!entityName) {
         if (typeof DawnToast !== 'undefined') DawnToast.show('Person name is required', 'error');
         nameInput.focus();
         return;
      }

      /* Photo-only save: name + photo but no contact value */
      if (!value && state.pendingPhotoId) {
         setSaveButtonEnabled(false);
         state.pendingAction = 'photo_only';
         if (state.selectedEntityId) {
            requestSetPhoto(state.selectedEntityId, state.pendingPhotoId);
            state.pendingPhotoId = null;
         } else {
            /* Create entity via upsert, then set photo on response */
            DawnWS.send({
               type: 'entity_ensure',
               payload: { name: entityName },
            });
         }
         return;
      }

      if (!value) {
         if (typeof DawnToast !== 'undefined') DawnToast.show('Value is required', 'error');
         valueInput.focus();
         return;
      }

      // Validate and normalize based on field type
      const normalizedValue = validateAndNormalize(fieldType, value);
      if (normalizedValue === null) {
         valueInput.focus();
         return;
      }
      // Update input with normalized value so user sees the canonical form
      valueInput.value = normalizedValue;

      setSaveButtonEnabled(false);
      if (state.editingContact) {
         requestUpdate(state.editingContact.contact_id, fieldType, normalizedValue, label);
      } else {
         const forceCreate = state.forceCreate || false;
         state.forceCreate = false;
         requestAdd(
            entityName,
            state.selectedEntityId,
            fieldType,
            normalizedValue,
            label,
            forceCreate
         );
      }
   }

   function setSaveButtonEnabled(enabled) {
      const btn = document.getElementById('contact-modal-save');
      if (btn) btn.disabled = !enabled;
   }

   let flashTimeout = null;
   function flashSaveButton() {
      const saveBtn = document.getElementById('contact-modal-save');
      if (!saveBtn) return;
      if (flashTimeout) {
         clearTimeout(flashTimeout);
         flashTimeout = null;
      }
      saveBtn.textContent = 'Saved!';
      saveBtn.classList.add('btn-saved');
      flashTimeout = setTimeout(() => {
         saveBtn.textContent = 'Save';
         saveBtn.classList.remove('btn-saved');
         flashTimeout = null;
      }, 1500);
   }

   /* =============================================================================
    * Photo Upload
    * ============================================================================= */

   function compressContactPhoto(file) {
      return new Promise((resolve, reject) => {
         const img = new Image();
         img.onload = () => {
            let { width, height } = img;
            if (width > PHOTO_MAX_DIM || height > PHOTO_MAX_DIM) {
               const scale = Math.min(PHOTO_MAX_DIM / width, PHOTO_MAX_DIM / height);
               width = Math.round(width * scale);
               height = Math.round(height * scale);
            }
            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const ctx = canvas.getContext('2d');
            ctx.fillStyle = '#1a1a2e';
            ctx.fillRect(0, 0, width, height);
            ctx.drawImage(img, 0, 0, width, height);
            canvas.toBlob(
               (blob) => (blob ? resolve(blob) : reject(new Error('toBlob failed'))),
               'image/jpeg',
               0.85
            );
            URL.revokeObjectURL(img.src);
         };
         img.onerror = () => {
            URL.revokeObjectURL(img.src);
            reject(new Error('Image load failed'));
         };
         img.src = URL.createObjectURL(file);
      });
   }

   async function handlePhotoUpload(file) {
      if (!file || !file.type.startsWith('image/')) {
         if (typeof DawnToast !== 'undefined') DawnToast.show('Not an image file', 'error');
         return;
      }
      if (file.size > 10 * 1024 * 1024) {
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Image too large (max 10 MB)', 'error');
         return;
      }

      const photoBtn = document.getElementById('contact-photo-btn');
      if (photoBtn) {
         photoBtn.textContent = 'Uploading...';
         photoBtn.disabled = true;
      }

      try {
         const compressed = await compressContactPhoto(file);

         const formData = new FormData();
         formData.append('image', compressed, 'contact-photo.jpg');
         const response = await fetch('/api/images', {
            method: 'POST',
            credentials: 'include',
            body: formData,
         });

         if (!response.ok) throw new Error('Upload failed: ' + response.status);
         const result = await response.json();

         // Show preview
         const preview = document.getElementById('contact-photo-preview');
         const placeholder = document.getElementById('contact-photo-placeholder');
         if (preview) {
            preview.src = '/api/images/' + encodeURIComponent(result.id);
            preview.classList.remove('hidden');
         }
         if (placeholder) placeholder.classList.add('hidden');
         showRemoveButton(true);

         // If editing an existing contact with a known entity, set the photo immediately
         if (state.editingContact && state.editingContact.entity_id) {
            requestSetPhoto(state.editingContact.entity_id, result.id);
         } else if (state.selectedEntityId) {
            requestSetPhoto(state.selectedEntityId, result.id);
         } else {
            // Store for later — will set after entity is created
            state.pendingPhotoId = result.id;
         }
      } catch (err) {
         console.error('Contact photo upload error:', err);
         if (typeof DawnToast !== 'undefined')
            DawnToast.show('Photo upload failed: ' + err.message, 'error');
      } finally {
         if (photoBtn) {
            photoBtn.textContent = 'Upload Photo';
            photoBtn.disabled = false;
         }
      }
   }

   function showRemoveButton(show) {
      const btn = document.getElementById('contact-photo-remove-btn');
      if (btn) {
         if (show) {
            btn.classList.remove('hidden');
         } else {
            btn.classList.add('hidden');
         }
      }
   }

   function handleRemovePhoto() {
      const entityId =
         (state.editingContact && state.editingContact.entity_id) || state.selectedEntityId;
      if (entityId) {
         state.pendingAction = 'photo_remove';
         requestSetPhoto(entityId, null);
      } else {
         /* No entity yet (new contact) — just clear the pending upload */
         resetPhotoUI();
      }
   }

   function resetPhotoUI() {
      const preview = document.getElementById('contact-photo-preview');
      const placeholder = document.getElementById('contact-photo-placeholder');
      if (preview) {
         preview.src = '';
         preview.classList.add('hidden');
      }
      if (placeholder) placeholder.classList.remove('hidden');
      showRemoveButton(false);
      state.pendingPhotoId = null;
   }

   /* =============================================================================
    * Typeahead — entity name search as user types
    * ============================================================================= */

   function handleEntityNameInput() {
      const nameInput = document.getElementById('contact-entity-name');
      if (!nameInput) return;

      const query = nameInput.value.trim();

      // Clear previous selection when user types
      state.selectedEntityId = null;

      if (state.typeaheadTimeout) clearTimeout(state.typeaheadTimeout);

      if (query.length < 2) {
         closeTypeaheadDropdown();
         updateEntityHint(query);
         return;
      }

      state.typeaheadTimeout = setTimeout(() => {
         requestSearchEntities(query);
      }, 200);
   }

   function renderTypeaheadDropdown() {
      const nameInput = document.getElementById('contact-entity-name');
      if (!nameInput) return;

      closeTypeaheadDropdown();

      const results = state.typeaheadResults;
      if (results.length === 0) {
         updateEntityHint(nameInput.value.trim());
         return;
      }

      const dropdown = document.createElement('div');
      dropdown.className = 'contact-typeahead-dropdown';
      dropdown.id = 'contact-typeahead-dropdown';

      results.forEach((entity) => {
         const item = document.createElement('div');
         item.className = 'contact-typeahead-item';
         item.dataset.entityId = entity.id;
         item.dataset.entityName = entity.name;
         item.innerHTML =
            '<span class="contact-typeahead-name">' +
            escapeHtml(entity.name) +
            '</span>' +
            '<span class="contact-typeahead-meta">' +
            escapeHtml(entity.entity_type) +
            ' &middot; ' +
            entity.mention_count +
            ' mentions</span>';
         item.addEventListener('click', () => selectTypeaheadEntity(entity));
         dropdown.appendChild(item);
      });

      // Position below the name input
      nameInput.parentNode.style.position = 'relative';
      nameInput.parentNode.appendChild(dropdown);
   }

   function selectTypeaheadEntity(entity) {
      const nameInput = document.getElementById('contact-entity-name');
      if (nameInput) nameInput.value = entity.name;

      state.selectedEntityId = entity.id;
      state.typeaheadResults = [];
      closeTypeaheadDropdown();

      const hint = document.getElementById('contact-entity-hint');
      if (hint) {
         hint.textContent = 'Using existing entity: ' + entity.name;
         hint.className = 'contact-entity-hint';
      }

      // Move focus to next field
      const valueInput = document.getElementById('contact-value');
      if (valueInput) valueInput.focus();
   }

   function closeTypeaheadDropdown() {
      const existing = document.getElementById('contact-typeahead-dropdown');
      if (existing) existing.remove();
   }

   function updateEntityHint(query) {
      const hint = document.getElementById('contact-entity-hint');
      if (!hint) return;

      if (!query || query.length === 0) {
         hint.classList.add('hidden');
      } else if (state.selectedEntityId) {
         hint.classList.remove('hidden');
      } else {
         hint.textContent = 'Will create new person entity';
         hint.className = 'contact-entity-hint new-entity';
      }
   }

   /* =============================================================================
    * Disambiguation — shown when server finds similar person entities
    * ============================================================================= */

   function showDisambiguation(candidates, enteredName) {
      const nameInput = document.getElementById('contact-entity-name');
      if (!nameInput) return;

      closeTypeaheadDropdown();

      const dropdown = document.createElement('div');
      dropdown.className = 'contact-typeahead-dropdown';
      dropdown.id = 'contact-typeahead-dropdown';

      // Header
      const header = document.createElement('div');
      header.className = 'contact-disambig-header';
      header.textContent = 'Similar people found — did you mean:';
      dropdown.appendChild(header);

      // Candidate items
      candidates.forEach((entity) => {
         const item = document.createElement('div');
         item.className = 'contact-typeahead-item';
         item.innerHTML =
            '<span class="contact-typeahead-name">' +
            escapeHtml(entity.name) +
            '</span>' +
            '<span class="contact-typeahead-meta">' +
            entity.mention_count +
            ' mentions</span>';
         item.addEventListener('click', () => {
            selectTypeaheadEntity(entity);
            // Re-submit with the selected entity
            handleModalSave();
         });
         dropdown.appendChild(item);
      });

      // "Create new" option
      const newItem = document.createElement('div');
      newItem.className = 'contact-typeahead-item';
      newItem.innerHTML =
         '<span class="contact-typeahead-name">Create new person &ldquo;' +
         escapeHtml(enteredName) +
         '&rdquo;</span>';
      newItem.addEventListener('click', () => {
         closeTypeaheadDropdown();
         // Force create by using entity_name path with a flag
         state.forceCreate = true;
         handleModalSave();
      });
      dropdown.appendChild(newItem);

      nameInput.parentNode.style.position = 'relative';
      nameInput.parentNode.appendChild(dropdown);
   }

   /**
    * Validate and normalize a contact value based on field type.
    * @returns {string|null} Normalized value, or null if invalid (toast shown).
    */
   function validateAndNormalize(fieldType, rawValue) {
      if (fieldType === 'email') {
         // Loose email regex — must have user@domain.tld
         if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(rawValue)) {
            if (typeof DawnToast !== 'undefined')
               DawnToast.show('Invalid email format (expected user@domain.com)', 'error');
            return null;
         }
         return rawValue.toLowerCase();
      }

      if (fieldType === 'phone') {
         // Strip formatting: spaces, dashes, parens, dots
         let digits = rawValue.replace(/[\s\-().]/g, '');

         // Reject invalid characters (anything not + or digit)
         if (/[^+0-9]/.test(digits)) {
            if (typeof DawnToast !== 'undefined')
               DawnToast.show('Phone number contains invalid characters', 'error');
            return null;
         }

         // Already has +, validate it
         if (digits.startsWith('+')) {
            if (!/^\+[0-9]{7,15}$/.test(digits)) {
               if (typeof DawnToast !== 'undefined')
                  DawnToast.show(
                     'Invalid phone number (expected + followed by 7-15 digits)',
                     'error'
                  );
               return null;
            }
            return digits;
         }

         // 11 digits starting with 1 — US number without +
         if (/^1[0-9]{10}$/.test(digits)) {
            return '+' + digits;
         }

         // 10 digits — US number, prepend +1
         if (/^[0-9]{10}$/.test(digits)) {
            return '+1' + digits;
         }

         if (typeof DawnToast !== 'undefined')
            DawnToast.show(
               'Invalid phone number. Enter 10-digit US number, or +country code with number.',
               'error'
            );
         return null;
      }

      // Address and other types — no validation needed
      return rawValue;
   }

   function updateValueInputType(fieldType) {
      const valueInput = document.getElementById('contact-value');
      if (!valueInput) return;

      switch (fieldType) {
         case 'email':
            valueInput.type = 'email';
            valueInput.placeholder = 'e.g. john@example.com';
            break;
         case 'phone':
            valueInput.type = 'tel';
            valueInput.placeholder = 'e.g. 5551234567 or +15551234567';
            break;
         case 'address':
            valueInput.type = 'text';
            valueInput.placeholder = 'e.g. 123 Main St, City';
            break;
      }
   }

   /* =============================================================================
    * Event Delegation
    * ============================================================================= */

   function handleListClick(e) {
      const actionEl = e.target.closest('[data-action]');
      if (!actionEl) return;

      const action = actionEl.dataset.action;
      const contactId = actionEl.dataset.contactId;

      switch (action) {
         case 'add-contact':
            openModal(null);
            break;
         case 'edit': {
            const contact = state.contacts.find((c) => String(c.contact_id) === String(contactId));
            if (contact) openModal(contact);
            break;
         }
         case 'delete':
            if (confirm('Delete this contact?')) {
               requestDelete(parseInt(contactId, 10));
            }
            break;
         case 'clear-filter':
            state.filterEntity = null;
            reload();
            break;
      }
   }

   /* =============================================================================
    * Public API (called from memory.js)
    * ============================================================================= */

   function loadContacts() {
      state.offset = 0;
      state.searchQuery = '';
      requestList(0);
   }

   function searchContacts(query) {
      if (!query || query.trim().length === 0) {
         loadContacts();
         return;
      }
      state.searchQuery = query;
      state.offset = 0;
      requestSearch(query);
   }

   function loadMore() {
      if (!state.hasMore || state.loading) return;
      state.offset += PAGE_SIZE;
      requestList(state.offset);
   }

   function filterByEntity(entityId, entityName) {
      state.filterEntity = { id: entityId, name: entityName };
      // Search by entity name to filter
      state.offset = 0;
      state.searchQuery = '';
      requestSearch(entityName);
   }

   function reload() {
      if (state.filterEntity) {
         requestSearch(state.filterEntity.name);
      } else {
         state.offset = 0;
         requestList(0);
      }
      // Also refresh stats
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'get_memory_stats' });
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      // Event delegation on memory-list
      const list = document.getElementById('memory-list');
      if (list) {
         list.addEventListener('click', handleListClick);
         list.addEventListener('error', handleThumbError, true); /* capture phase for img errors */
      }

      // Modal events
      const closeBtn = document.getElementById('contact-modal-close');
      const cancelBtn = document.getElementById('contact-modal-cancel');
      const saveBtn = document.getElementById('contact-modal-save');
      const typeSelect = document.getElementById('contact-field-type');
      const modal = document.getElementById('contact-modal');

      if (closeBtn) closeBtn.addEventListener('click', closeModal);
      if (cancelBtn) cancelBtn.addEventListener('click', closeModal);
      if (saveBtn) saveBtn.addEventListener('click', handleModalSave);
      if (typeSelect) {
         typeSelect.addEventListener('change', () => updateValueInputType(typeSelect.value));
      }
      // Close on overlay click
      if (modal) {
         modal.addEventListener('click', (e) => {
            if (e.target === modal) closeModal();
         });
      }

      // Entity name typeahead
      const nameInput = document.getElementById('contact-entity-name');
      if (nameInput) {
         nameInput.addEventListener('input', handleEntityNameInput);
         // Close dropdown when focus leaves the name input area
         nameInput.addEventListener('blur', () => {
            // Delay to allow click on dropdown item
            setTimeout(closeTypeaheadDropdown, 200);
         });
      }

      // Photo upload
      const photoBtn = document.getElementById('contact-photo-btn');
      const photoInput = document.getElementById('contact-photo-input');
      const photoCircle = document.getElementById('contact-photo-circle');
      if (photoBtn && photoInput) {
         photoBtn.addEventListener('click', () => photoInput.click());
         if (photoCircle) {
            photoCircle.addEventListener('click', () => photoInput.click());
            photoCircle.addEventListener('keydown', (e) => {
               if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  photoInput.click();
               }
            });
         }
         photoInput.addEventListener('change', (e) => {
            if (e.target.files && e.target.files[0]) {
               handlePhotoUpload(e.target.files[0]);
            }
            e.target.value = '';
         });
      }

      // Photo remove
      const removeBtn = document.getElementById('contact-photo-remove-btn');
      if (removeBtn) removeBtn.addEventListener('click', handleRemovePhoto);

      console.log('DawnContacts: Initialized');
   }

   /* =============================================================================
    * Helpers
    * ============================================================================= */

   function escapeHtml(text) {
      if (!text) return '';
      return String(text)
         .replace(/&/g, '&amp;')
         .replace(/</g, '&lt;')
         .replace(/>/g, '&gt;')
         .replace(/"/g, '&quot;');
   }

   /* =============================================================================
    * Export
    * ============================================================================= */

   window.DawnContacts = {
      init,
      loadContacts,
      searchContacts,
      loadMore,
      filterByEntity,
      // Response handlers (called from dawn.js dispatch)
      handleListResponse,
      handleSearchResponse,
      handleAddResponse,
      handleUpdateResponse,
      handleDeleteResponse,
      handleSearchEntitiesResponse,
      handleSetPhotoResponse,
      handleEnsureEntityResponse,
   };
})();
