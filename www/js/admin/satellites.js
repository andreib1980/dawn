/**
 * DAWN Satellite Management Module
 * Admin satellite CRUD operations and UI
 */
(function () {
   'use strict';

   let satellites = [];
   let users = [];
   let haAreas = [];
   let refreshInterval = null;
   let callbacks = {
      showConfirmModal: null,
   };

   const REFRESH_INTERVAL_MS = 30000;
   const LOCAL_PSEUDO_UUID = '00000000-0000-0000-0000-000000000000';

   /* =============================================================================
    * API Requests
    * ============================================================================= */

   function requestListSatellites() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         const list = document.getElementById('satellite-list');
         if (list && satellites.length === 0) {
            list.innerHTML = '<div class="loading-indicator">Loading satellites...</div>';
         }
         DawnWS.send({ type: 'list_satellites' });
      }
   }

   function requestUpdateSatellite(uuid, updates) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'update_satellite',
            payload: { uuid, ...updates },
         });
      }
   }

   function requestDeleteSatellite(uuid) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'delete_satellite',
            payload: { uuid },
         });
      }
   }

   /* =============================================================================
    * Time Helpers
    * ============================================================================= */

   function formatLastSeen(timestamp) {
      if (!timestamp) return 'Never';
      const now = Math.floor(Date.now() / 1000);
      const diff = now - timestamp;
      if (diff < 60) return 'Just now';
      if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
      if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
      return Math.floor(diff / 86400) + 'd ago';
   }

   /* =============================================================================
    * UI Rendering
    * ============================================================================= */

   function buildHaAreaControl(sat) {
      if (haAreas.length > 0) {
         // Dropdown from HA areas
         let options = '<option value="">-- No Area --</option>';
         for (const area of haAreas) {
            const selected = area === (sat.ha_area || '') ? ' selected' : '';
            options += '<option value="' + escapeHtml(area) + '"' + selected + '>';
            options += escapeHtml(area) + '</option>';
         }
         // Add current value if not in list (manually set previously)
         if (sat.ha_area && !haAreas.includes(sat.ha_area)) {
            options +=
               '<option value="' +
               escapeHtml(sat.ha_area) +
               '" selected>' +
               escapeHtml(sat.ha_area) +
               '</option>';
         }
         return (
            '<select class="satellite-ha-area" data-uuid="' +
            sat.uuid +
            '">' +
            options +
            '</select>'
         );
      }
      // Fallback: text input when HA not configured
      return (
         '<input type="text" class="satellite-ha-area" data-uuid="' +
         sat.uuid +
         '" value="' +
         escapeHtml(sat.ha_area || '') +
         '" placeholder="e.g., Living Room">'
      );
   }

   function renderSatelliteList() {
      const list = document.getElementById('satellite-list');
      if (!list) return;

      if (satellites.length === 0) {
         list.innerHTML =
            '<div class="satellite-list-empty">' +
            'No satellites registered. Satellites appear here automatically when they connect.' +
            '</div>';
         return;
      }

      let html = '';
      for (const sat of satellites) {
         const isLocal = sat.uuid === LOCAL_PSEUDO_UUID;
         let tierLabel, tierClass;
         if (isLocal) {
            tierLabel = 'LOCAL';
            tierClass = 'tier-local';
         } else if (sat.tier === 1) {
            tierLabel = 'T1 RPi';
            tierClass = 'tier-1';
         } else {
            tierLabel = 'T2 ESP32';
            tierClass = 'tier-2';
         }
         // Local pseudo-satellite is always "present" — treat as online.
         const effectiveOnline = isLocal ? true : sat.online;
         const statusClass = effectiveOnline ? 'online' : 'offline';
         const statusLabel = effectiveOnline ? 'Online' : 'Offline';
         const lastSeenText =
            isLocal || effectiveOnline ? '' : 'Last seen: ' + formatLastSeen(sat.last_seen);

         // Build user dropdown options
         let userOptions = '<option value="0">-- Unassigned --</option>';
         for (const u of users) {
            const selected = u.id === sat.user_id ? ' selected' : '';
            userOptions +=
               '<option value="' +
               u.id +
               '"' +
               selected +
               '>' +
               escapeHtml(u.display_name) +
               '</option>';
         }

         html +=
            '<div class="satellite-card" data-uuid="' +
            sat.uuid +
            '">' +
            // Header row
            '<div class="satellite-header">' +
            '<span class="satellite-status ' +
            statusClass +
            '" title="' +
            statusLabel +
            '" role="img" aria-label="' +
            statusLabel +
            '"></span>' +
            '<span class="satellite-name">' +
            escapeHtml(sat.name) +
            '</span>' +
            '<span class="satellite-tier ' +
            tierClass +
            '">' +
            tierLabel +
            '</span>' +
            '<span class="satellite-location">' +
            escapeHtml(sat.location || 'No location') +
            '</span>' +
            '<span class="satellite-status-text ' +
            statusClass +
            '">' +
            statusLabel +
            '</span>' +
            '</div>' +
            (lastSeenText ? '<div class="satellite-last-seen">' + lastSeenText + '</div>' : '') +
            // Controls row
            '<div class="satellite-controls">' +
            '<label class="satellite-control-group">' +
            '<span class="control-label">User:</span>' +
            '<select class="satellite-user-select" data-uuid="' +
            sat.uuid +
            '">' +
            userOptions +
            '</select>' +
            '</label>' +
            '<label class="satellite-control-group">' +
            '<span class="control-label">HA Area:</span>' +
            buildHaAreaControl(sat) +
            '</label>' +
            '</div>' +
            // Footer: delete button (hidden for the daemon's local pseudo-satellite)
            (isLocal
               ? '<div class="satellite-footer satellite-footer-local">' +
                 '<span class="satellite-local-note">' +
                 'Daemon speaker. <strong>Unassigned</strong> plays for everyone; ' +
                 'assign to a user to restrict notifications to them. Cannot be deleted.' +
                 '</span>' +
                 '</div>'
               : '<div class="satellite-footer">' +
                 '<button class="btn satellite-delete-btn" data-uuid="' +
                 sat.uuid +
                 '" data-name="' +
                 escapeHtml(sat.name) +
                 '" data-user="' +
                 escapeHtml(getUserName(sat.user_id)) +
                 '">Delete Satellite</button>' +
                 '</div>') +
            '</div>';
      }

      list.innerHTML = html;
      attachEventListeners();
   }

   function getUserName(userId) {
      if (!userId) return 'Unassigned';
      const user = users.find((u) => u.id === userId);
      return user ? user.display_name : 'Unknown';
   }

   function escapeHtml(str) {
      return DawnFormat.escapeHtml(str);
   }

   /* =============================================================================
    * Event Listeners
    * ============================================================================= */

   function attachEventListeners() {
      // User assignment dropdown
      document.querySelectorAll('.satellite-user-select').forEach((sel) => {
         sel.addEventListener('change', function () {
            const uuid = this.dataset.uuid;
            const userId = parseInt(this.value, 10);
            this.disabled = true;
            requestUpdateSatellite(uuid, { user_id: userId });
         });
      });

      // HA area (works for both select and input)
      document.querySelectorAll('.satellite-ha-area').forEach((el) => {
         if (el.tagName === 'SELECT') {
            el.addEventListener('change', function () {
               const uuid = this.dataset.uuid;
               this.disabled = true;
               requestUpdateSatellite(uuid, { ha_area: this.value });
            });
         } else {
            el.addEventListener('blur', function () {
               const uuid = this.dataset.uuid;
               const sat = satellites.find((s) => s.uuid === uuid);
               if (sat && this.value !== (sat.ha_area || '')) {
                  this.disabled = true;
                  requestUpdateSatellite(uuid, { ha_area: this.value });
               }
            });
            el.addEventListener('keydown', function (e) {
               if (e.key === 'Enter') this.blur();
            });
         }
      });

      // Delete buttons
      document.querySelectorAll('.satellite-delete-btn').forEach((btn) => {
         btn.addEventListener('click', function () {
            const uuid = this.dataset.uuid;
            const name = this.dataset.name;
            const assignedUser = this.dataset.user;
            let msg = "Delete satellite '" + name + "'?";
            if (assignedUser && assignedUser !== 'Unassigned') {
               msg += "\nCurrently assigned to user '" + assignedUser + "'.";
            }
            msg += '\nThis satellite will need to re-register to appear again.';
            if (callbacks.showConfirmModal) {
               callbacks.showConfirmModal(
                  msg,
                  () => {
                     requestDeleteSatellite(uuid);
                  },
                  { title: 'Delete Satellite', okText: 'Delete', danger: true }
               );
            } else if (confirm(msg)) {
               requestDeleteSatellite(uuid);
            }
         });
      });
   }

   /* =============================================================================
    * Auto-Refresh
    * ============================================================================= */

   function startAutoRefresh() {
      stopAutoRefresh();
      refreshInterval = setInterval(requestListSatellites, REFRESH_INTERVAL_MS);
   }

   function stopAutoRefresh() {
      if (refreshInterval) {
         clearInterval(refreshInterval);
         refreshInterval = null;
      }
   }

   /* =============================================================================
    * Response Handlers
    * ============================================================================= */

   function handleListResponse(payload) {
      if (payload.satellites) {
         satellites = payload.satellites;
      }
      if (payload.users) {
         users = payload.users;
      }
      if (payload.ha_areas) {
         haAreas = payload.ha_areas;
      }
      renderSatelliteList();
   }

   function handleUpdateResponse(payload) {
      if (payload.success && payload.satellite) {
         const idx = satellites.findIndex((s) => s.uuid === payload.satellite.uuid);
         if (idx >= 0) {
            satellites[idx] = payload.satellite;
         }
         renderSatelliteList();
      } else {
         renderSatelliteList(); // Re-enable controls
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to update satellite', 'error');
         }
      }
   }

   function handleDeleteResponse(payload) {
      if (payload.success && payload.uuid) {
         satellites = satellites.filter((s) => s.uuid !== payload.uuid);
         renderSatelliteList();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Satellite deleted', 'success');
         }
      } else {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to delete satellite', 'error');
         }
      }
   }

   /* =============================================================================
    * Initialization
    * ============================================================================= */

   function init() {
      const section = document.getElementById('satellite-management-section');
      if (!section) return;

      // Load on section expand and manage auto-refresh
      // (toggle handled by generic handler in settings.js)
      const header = section.querySelector('.section-header');
      if (header) {
         header.addEventListener('click', function () {
            // Check after the generic handler has toggled 'collapsed'
            setTimeout(function () {
               if (!section.classList.contains('collapsed')) {
                  if (satellites.length === 0) {
                     requestListSatellites();
                  }
                  startAutoRefresh();
               } else {
                  stopAutoRefresh();
               }
            }, 0);
         });
      }

      // Refresh button
      const refreshBtn = document.getElementById('refresh-satellites-btn');
      if (refreshBtn) {
         refreshBtn.addEventListener('click', requestListSatellites);
      }
   }

   // Initialize when DOM is ready
   if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', init);
   } else {
      init();
   }

   /* =============================================================================
    * Public API
    * ============================================================================= */

   window.DawnSatellites = {
      handleListResponse: handleListResponse,
      handleUpdateResponse: handleUpdateResponse,
      handleDeleteResponse: handleDeleteResponse,
      handleReconnect: function () {
         /* Re-render to un-disable any stuck controls, then refresh if section is open */
         renderSatelliteList();
         const section = document.getElementById('satellite-management-section');
         if (section && !section.classList.contains('collapsed') && satellites.length > 0) {
            requestListSatellites();
         }
      },
      refresh: requestListSatellites,
      stopAutoRefresh: stopAutoRefresh,
      setCallbacks: function (cbs) {
         if (cbs && cbs.showConfirmModal) callbacks.showConfirmModal = cbs.showConfirmModal;
      },
   };
})();
