/**
 * DAWN Satellite Management Module
 * Admin satellite CRUD operations and UI
 */
(function () {
   'use strict';

   let satellites = [];
   let users = [];
   let haAreas = [];
   let otaReleases = []; // [{ platform, tier, version }]
   let otaEnabled = false;
   let refreshInterval = null;
   // Fleet-rollout panel state (persists across the 30s list re-render so an
   // active rollout's status line isn't lost).
   let fleetType = 'esp32'; // selected platform for push-all
   let fleetStatusText = ''; // last rollout status line from the daemon
   let fleetPollTimer = null; // status poll while a rollout is in flight
   let callbacks = {
      trapFocus: null,
   };

   const REFRESH_INTERVAL_MS = 30000;
   const FLEET_POLL_INTERVAL_MS = 4000; // status poll cadence during a rollout
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

   function requestOtaList() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'ota_list' });
      }
   }

   function requestOtaPush(uuid, version, allowDowngrade) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'ota_push',
            payload: { uuid, version, allow_downgrade: !!allowDowngrade },
         });
      }
   }

   function requestOtaPushAll(platform, version, allowDowngrade) {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({
            type: 'ota_push_all',
            payload: { platform, version, allow_downgrade: !!allowDowngrade },
         });
      }
   }

   function requestRolloutStatus() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'ota_rollout_status' });
      }
   }

   function requestRolloutAbort() {
      if (typeof DawnWS !== 'undefined' && DawnWS.isConnected()) {
         DawnWS.send({ type: 'ota_rollout_abort' });
      }
   }

   // Tier → release platform string (tier 1 = RPi, tier 2 = ESP32).
   function platformForTier(tier) {
      return tier === 2 ? 'esp32' : 'rpi';
   }

   // Versions available for a platform string (newest first by store order).
   function releaseVersionsForPlatform(platform) {
      return otaReleases.filter((r) => r.platform === platform).map((r) => r.version);
   }

   // A rollout is "active" (still working) when its status line names a
   // non-terminal state — used to gate the Abort button + status polling.
   function rolloutIsActive(statusText) {
      return /—\s*(starting|waiting on canary|rolling out)\b/.test(statusText || '');
   }

   function startFleetPolling() {
      stopFleetPolling();
      fleetPollTimer = setInterval(requestRolloutStatus, FLEET_POLL_INTERVAL_MS);
   }

   function stopFleetPolling() {
      if (fleetPollTimer) {
         clearInterval(fleetPollTimer);
         fleetPollTimer = null;
      }
   }

   // Available release versions for a device's platform (newest first by the
   // server's store order, which is already version-sorted).
   function releaseVersionsForTier(tier) {
      const platform = platformForTier(tier);
      return otaReleases.filter((r) => r.platform === platform).map((r) => r.version);
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

   // OTA push control for an eligible device: a version picker + allow-downgrade
   // toggle + Update button.  Returns '' when OTA is disabled, the device is
   // offline/local, or no release matches the device's platform.
   function buildOtaControl(sat, isLocal, online) {
      if (isLocal || !online || !otaEnabled) return '';
      const versions = releaseVersionsForTier(sat.tier);
      if (versions.length === 0) return '';
      // "busy" mirrors the server's OTA_INFLIGHT_PREDICATE exactly (an update is
      // actively in flight).  Everything else — idle/success/failed AND 'unknown'
      // (state lost to a daemon restart, needs reconciliation) — is re-pushable.
      const OTA_INFLIGHT = ['offered', 'downloading', 'verifying', 'applying', 'rebooting'];
      const busy = OTA_INFLIGHT.includes(sat.ota_state);
      // escapeAttr (not escapeHtml) for attribute context — escapeHtml does not
      // escape quotes, and sat.name is attacker-controlled (set at registration
      // with no charset restriction).  See SEC review 2026-06-08.
      let options = '';
      for (const v of versions) {
         options += '<option value="' + escapeAttr(v) + '">' + escapeHtml(v) + '</option>';
      }
      return (
         '<div class="satellite-controls satellite-ota-controls">' +
         '<label class="satellite-control-group">' +
         '<span class="control-label">Update to:</span>' +
         '<select class="satellite-ota-version" aria-label="Update version" data-uuid="' +
         escapeAttr(sat.uuid) +
         '"' +
         (busy ? ' disabled' : '') +
         '>' +
         options +
         '</select>' +
         '</label>' +
         '<label class="satellite-control-group satellite-ota-downgrade">' +
         '<input type="checkbox" class="satellite-ota-allow-downgrade" data-uuid="' +
         escapeAttr(sat.uuid) +
         '">' +
         '<span class="control-label">Allow downgrade</span>' +
         '</label>' +
         '<button class="btn satellite-ota-push-btn" data-uuid="' +
         escapeAttr(sat.uuid) +
         '" data-name="' +
         escapeAttr(sat.name) +
         '"' +
         (busy ? ' disabled title="Update already in progress"' : '') +
         '>Push Update</button>' +
         '</div>'
      );
   }

   // Fleet rollout panel: pick a satellite type + version, roll it out to the
   // whole fleet (canary first).  Rendered at the bottom of the satellite list.
   // Hidden when OTA is disabled or no release exists for any platform.
   function buildFleetRolloutPanel() {
      if (!otaEnabled) return '';
      const platforms = [];
      if (releaseVersionsForPlatform('esp32').length > 0) platforms.push('esp32');
      if (releaseVersionsForPlatform('rpi').length > 0) platforms.push('rpi');
      if (platforms.length === 0) return '';
      if (platforms.indexOf(fleetType) === -1) fleetType = platforms[0];

      let typeOpts = '';
      for (const p of platforms) {
         const label = p === 'esp32' ? 'ESP32 (Tier 2)' : 'Raspberry Pi (Tier 1)';
         typeOpts +=
            '<option value="' +
            p +
            '"' +
            (p === fleetType ? ' selected' : '') +
            '>' +
            label +
            '</option>';
      }
      let verOpts = '';
      for (const v of releaseVersionsForPlatform(fleetType)) {
         verOpts += '<option value="' + escapeAttr(v) + '">' + escapeHtml(v) + '</option>';
      }

      const active = rolloutIsActive(fleetStatusText);
      const statusLine = fleetStatusText || 'No rollout has run.';
      return (
         '<div class="satellite-fleet-rollout">' +
         '<h4 class="satellite-fleet-rollout-title">Fleet Rollout</h4>' +
         '<p class="satellite-fleet-rollout-desc">Roll a release out to every online device of a type — ' +
         'one canary first; the rest follow only after it updates and re-registers successfully.</p>' +
         '<div class="satellite-controls">' +
         '<label class="satellite-control-group">' +
         '<span class="control-label">Type:</span>' +
         // aria-label: the .control-label <span> is styled text, not a wired
         // <label for>, so the select needs its own accessible name.
         '<select class="fleet-rollout-type" aria-label="Satellite type">' +
         typeOpts +
         '</select>' +
         '</label>' +
         '<label class="satellite-control-group">' +
         '<span class="control-label">Version:</span>' +
         '<select class="fleet-rollout-version" aria-label="Release version">' +
         verOpts +
         '</select>' +
         '</label>' +
         '<label class="satellite-control-group satellite-ota-downgrade">' +
         '<input type="checkbox" class="fleet-rollout-allow-downgrade">' +
         '<span class="control-label">Allow downgrade</span>' +
         '</label>' +
         '<button class="btn fleet-rollout-start-btn">Roll Out</button>' +
         '</div>' +
         '<div class="satellite-fleet-rollout-status">' +
         '<span class="control-label">Status:</span> ' +
         // aria-live: the 4s poll mutates this via textContent — announce progress.
         '<span id="fleet-rollout-status-text" aria-live="polite" aria-atomic="true">' +
         escapeHtml(statusLine) +
         '</span>' +
         '<button class="btn fleet-rollout-refresh-btn">Refresh</button>' +
         (active ? '<button class="btn fleet-rollout-abort-btn">Abort</button>' : '') +
         '</div>' +
         '</div>'
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
            // Firmware version + any in-flight OTA state (non-local devices that have reported it)
            (!isLocal && sat.firmware_version
               ? '<div class="satellite-last-seen satellite-firmware">Firmware: ' +
                 escapeHtml(sat.firmware_version) +
                 (sat.ota_state && sat.ota_state !== 'idle'
                    ? ' · ' +
                      escapeHtml(sat.ota_state) +
                      (sat.ota_target_version ? ' → ' + escapeHtml(sat.ota_target_version) : '')
                    : '') +
                 '</div>'
               : '') +
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
            // OTA push control (eligible online devices only)
            buildOtaControl(sat, isLocal, effectiveOnline) +
            // Footer: delete button (hidden for the daemon's local pseudo-satellite)
            (isLocal
               ? '<div class="satellite-footer satellite-footer-local">' +
                 '<span class="satellite-local-note">' +
                 'Daemon speaker. <strong>Unassigned</strong> plays for everyone; ' +
                 'assign to a user to restrict notifications to them. Cannot be deleted.' +
                 '</span>' +
                 '</div>'
               : '<div class="satellite-footer">' +
                 // escapeAttr for attribute context (sat.name is attacker-controlled).
                 '<button class="btn satellite-delete-btn" data-uuid="' +
                 escapeAttr(sat.uuid) +
                 '" data-name="' +
                 escapeAttr(sat.name) +
                 '" data-user="' +
                 escapeAttr(getUserName(sat.user_id)) +
                 '">Delete Satellite</button>' +
                 '</div>') +
            '</div>';
      }

      html += buildFleetRolloutPanel();
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

   // For interpolation inside attribute="..." values (escapes quotes too).
   function escapeAttr(str) {
      return DawnFormat.escapeAttr(str);
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
         btn.addEventListener('click', async function () {
            const uuid = this.dataset.uuid;
            const name = this.dataset.name;
            const assignedUser = this.dataset.user;
            let msg = "Delete satellite '" + name + "'?";
            if (assignedUser && assignedUser !== 'Unassigned') {
               msg += "\nCurrently assigned to user '" + assignedUser + "'.";
            }
            msg += '\nThis satellite will need to re-register to appear again.';
            if (
               await DawnDialog.confirm(msg, {
                  title: 'Delete Satellite',
                  okText: 'Delete',
                  danger: true,
               })
            ) {
               requestDeleteSatellite(uuid);
            }
         });
      });

      // OTA push buttons
      document.querySelectorAll('.satellite-ota-push-btn').forEach((btn) => {
         btn.addEventListener('click', async function () {
            const uuid = this.dataset.uuid;
            const name = this.dataset.name;
            const card = this.closest('.satellite-card');
            const versionSel = card && card.querySelector('.satellite-ota-version');
            const downgradeCb = card && card.querySelector('.satellite-ota-allow-downgrade');
            const version = versionSel ? versionSel.value : '';
            const allowDowngrade = downgradeCb ? downgradeCb.checked : false;
            if (!version) {
               if (typeof DawnToast !== 'undefined') {
                  DawnToast.show('No release version selected', 'error');
               }
               return;
            }
            let msg = 'Push update ' + version + " to '" + name + "'?";
            if (allowDowngrade) {
               msg += '\nDowngrade is allowed for this push.';
            }
            msg += '\nThe device will download, verify, and apply it.';
            const doPush = () => {
               this.disabled = true;
               requestOtaPush(uuid, version, allowDowngrade);
            };
            if (await DawnDialog.confirm(msg, { title: 'Push OTA Update', okText: 'Push' })) {
               doPush();
            }
         });
      });

      // Fleet rollout panel: type switch repopulates versions in place; Roll Out /
      // Abort confirm via DawnDialog (Promise-based styled confirm).
      const fleetTypeSel = document.querySelector('.fleet-rollout-type');
      if (fleetTypeSel) {
         fleetTypeSel.addEventListener('change', function () {
            fleetType = this.value;
            const verSel = document.querySelector('.fleet-rollout-version');
            if (verSel) {
               let opts = '';
               for (const v of releaseVersionsForPlatform(fleetType)) {
                  opts += '<option value="' + escapeAttr(v) + '">' + escapeHtml(v) + '</option>';
               }
               verSel.innerHTML = opts;
            }
         });
      }

      const fleetStartBtn = document.querySelector('.fleet-rollout-start-btn');
      if (fleetStartBtn) {
         fleetStartBtn.addEventListener('click', async function () {
            const typeSel = document.querySelector('.fleet-rollout-type');
            const verSel = document.querySelector('.fleet-rollout-version');
            const dgCb = document.querySelector('.fleet-rollout-allow-downgrade');
            const platform = typeSel ? typeSel.value : fleetType;
            const version = verSel ? verSel.value : '';
            const allowDowngrade = dgCb ? dgCb.checked : false;
            if (!version) return;
            const typeLabel = platform === 'esp32' ? 'ESP32' : 'Raspberry Pi';
            let msg =
               'Roll out ' +
               version +
               ' to ALL online ' +
               typeLabel +
               ' devices?\n\n' +
               'A canary goes first; the rest follow only if it updates successfully.';
            if (allowDowngrade) msg += '\n\nDowngrade is ALLOWED for this rollout.';
            const doRollout = function () {
               // Guard against a double-submit of this high-blast-radius action;
               // the next renderSatelliteList() recreates the button enabled.
               fleetStartBtn.disabled = true;
               requestOtaPushAll(platform, version, allowDowngrade);
            };
            if (
               await DawnDialog.confirm(msg, {
                  title: 'Fleet Rollout',
                  okText: 'Roll Out',
                  danger: true, // whole-fleet action — render OK in the danger style
               })
            ) {
               doRollout();
            }
         });
      }

      const fleetRefreshBtn = document.querySelector('.fleet-rollout-refresh-btn');
      if (fleetRefreshBtn) {
         fleetRefreshBtn.addEventListener('click', requestRolloutStatus);
      }

      const fleetAbortBtn = document.querySelector('.fleet-rollout-abort-btn');
      if (fleetAbortBtn) {
         fleetAbortBtn.addEventListener('click', async function () {
            const msg = 'Abort the in-progress rollout? Devices not yet offered are skipped.';
            if (await DawnDialog.confirm(msg, { title: 'Abort Rollout', okText: 'Abort' })) {
               requestRolloutAbort();
            }
         });
      }
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
      stopFleetPolling();
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

   function handleOtaListResponse(payload) {
      if (!payload) return;
      otaEnabled = !!payload.enabled;
      otaReleases = Array.isArray(payload.releases) ? payload.releases : [];
      renderSatelliteList();
      // Reflect any rollout already in flight (e.g. after a page reload).
      if (otaEnabled) {
         requestRolloutStatus();
      }
   }

   function handleOtaPushResponse(payload) {
      if (payload && payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Update ' + (payload.version || '') + ' pushed to satellite', 'success');
         }
         // Refresh so the in-flight ota_state shows on the card.
         requestListSatellites();
      } else {
         // Defensive fallback: the daemon reports push failures via a generic
         // {type:error, code:OTA_ERROR} (handled in dawn.js -> handleOtaError),
         // not an ota_push_response with success:false, so this branch is a
         // safety net in case that contract ever changes.
         handleOtaError();
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Failed to push update', 'error');
         }
      }
   }

   // A push / rollout request failed: the daemon replies with a generic
   // {type:error, code:OTA_ERROR}, which dawn.js routes here.  Re-render so the
   // disabled Push / Roll Out buttons become usable again immediately instead of
   // staying dead until the next 30 s auto-refresh.  (dawn.js shows the toast.)
   function handleOtaError() {
      renderSatelliteList();
   }

   // Update only the status text in place (avoids a full re-render that would
   // reset the panel's selections); re-render only on an active↔idle transition
   // so the Abort button appears/disappears.
   function updateFleetStatusDisplay() {
      const el = document.getElementById('fleet-rollout-status-text');
      if (!el) return; // panel not in the DOM (section collapsed / OTA off)
      el.textContent = fleetStatusText || 'No rollout has run.';
      const hasAbort = !!document.querySelector('.fleet-rollout-abort-btn');
      if (rolloutIsActive(fleetStatusText) !== hasAbort) {
         renderSatelliteList();
      }
   }

   function handleOtaPushAllResponse(payload) {
      if (payload && payload.success) {
         fleetStatusText = payload.summary || '';
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.summary || 'Rollout started', 'success');
         }
         updateFleetStatusDisplay();
         startFleetPolling();
      }
      // Failures arrive via the generic OTA_ERROR path (toast), like ota_push.
   }

   function handleOtaRolloutStatusResponse(payload) {
      if (!payload) return;
      fleetStatusText = typeof payload.status === 'string' ? payload.status : '';
      updateFleetStatusDisplay();
      if (rolloutIsActive(fleetStatusText)) {
         // A rollout is in flight but THIS tab didn't start it (page reload,
         // section reopen, or a second admin tab): startFleetPolling was never
         // called, so resume polling instead of leaving the status frozen.
         if (!fleetPollTimer) {
            startFleetPolling();
         }
      } else {
         stopFleetPolling(); // terminal/idle — no need to keep polling
      }
   }

   function handleOtaRolloutAbortResponse(payload) {
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            payload && payload.aborted ? 'Rollout aborted' : 'No rollout in progress',
            'info'
         );
      }
      requestRolloutStatus(); // pull the post-abort status line
   }

   /* =============================================================================
    * Pairing Modal (QR-code surface for Android satellite onboarding)
    * ============================================================================= */

   /* dawn://provision URI schema version.  Increment when the URI parameter
    * shape changes; the Android app rejects unknown versions so a bump forces
    * an explicit acknowledgement on the client side. */
   const PAIR_URI_VERSION = '1';
   const PAIR_IDLE_TIMEOUT_MS = 5 * 60 * 1000; /* auto-close modal after 5 min idle */
   const PAIR_CLIPBOARD_CLEAR_MS = 30 * 1000; /* wipe clipboard 30s after copy */
   const PAIR_RERENDER_DEBOUNCE_MS = 200;
   const PAIR_REVEAL_AUTO_HIDE_MS = 30 * 1000; /* re-mask the key 30s after reveal */

   const pairState = {
      key: '',
      tlsEnabled: false,
      revealed: false,
      lastRenderedUri: '' /* skip identical QR re-renders */,
      focusCleanup: null,
      idleTimer: null,
      clipboardClearTimer: null,
      rerenderTimer: null,
      revealAutoHideTimer: null,
      keydownCleanup: null,
      previousFocus: null,
   };

   /* Cached DOM references — populated once in initPairModal and reused on
    * every modal open.  Avoids 12 getElementById lookups per resetIdleTimer
    * tick (mousemove fires those frequently in long sessions). */
   let pairEls = null;

   function buildPairEls() {
      return {
         modal: document.getElementById('satellite-pair-modal'),
         closeBtn: document.getElementById('satellite-pair-close'),
         qr: document.getElementById('satellite-pair-qr'),
         qrWrap: document.getElementById('satellite-pair-qr-wrap'),
         empty: document.getElementById('satellite-pair-empty'),
         server: document.getElementById('satellite-pair-server'),
         wsWarning: document.getElementById('satellite-pair-ws-warning'),
         keyInput: document.getElementById('satellite-pair-key'),
         keyReveal: document.getElementById('satellite-pair-key-reveal'),
         copyBtn: document.getElementById('satellite-pair-copy'),
         copyStatus: document.getElementById('satellite-pair-copy-status'),
         pairBtn: document.getElementById('pair-satellite-btn'),
      };
   }

   function defaultServerUrl() {
      const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      return scheme + '//' + window.location.host + '/ws';
   }

   function buildPairUri(serverUrl, key) {
      return (
         'dawn://provision?server=' +
         encodeURIComponent(serverUrl) +
         '&v=' +
         PAIR_URI_VERSION +
         '#key=' +
         encodeURIComponent(key)
      );
   }

   function renderQrInto(container, uri) {
      /* qrcode-generator API: type=0 (auto), error-correction 'M' fits ~120-char URIs
       * comfortably. createSvgTag returns an XML-escaped SVG string with black
       * modules on a white background — both required for reliable phone scans
       * against a dark-themed modal.
       *
       * XSS-safe: the URI is bitstream-encoded into Reed-Solomon QR modules and
       * rendered as SVG <rect> elements; input never lands inside an SVG
       * attribute value, so the inputs (server URL + key, both URL-encoded)
       * cannot escape the rendering context. */
      const qr = qrcode(0, 'M');
      qr.addData(uri);
      qr.make();
      container.innerHTML = qr.createSvgTag({ cellSize: 6, margin: 4, scalable: true });
   }

   function scheduleRerender() {
      if (pairState.rerenderTimer) clearTimeout(pairState.rerenderTimer);
      pairState.rerenderTimer = setTimeout(rerenderPairModal, PAIR_RERENDER_DEBOUNCE_MS);
   }

   function setCopyButtonDisabled(disabled, reason) {
      if (!pairEls || !pairEls.copyBtn) return;
      pairEls.copyBtn.disabled = disabled;
      if (disabled && reason) {
         pairEls.copyBtn.setAttribute('aria-describedby', reason);
      } else {
         pairEls.copyBtn.removeAttribute('aria-describedby');
      }
   }

   function rerenderPairModal() {
      if (!pairEls || pairEls.modal.classList.contains('hidden')) return;
      if (!pairState.key) return;
      const serverUrl = (pairEls.server.value || '').trim();
      const isWs = /^ws:\/\//i.test(serverUrl);
      const tlsDowngrade = isWs && pairState.tlsEnabled;
      if (pairEls.wsWarning) {
         pairEls.wsWarning.classList.toggle('hidden', !tlsDowngrade);
      }
      /* When the daemon is serving TLS, refuse to emit a downgraded ws:// payload —
       * the PSK would travel cleartext on the satellite's next register. */
      if (tlsDowngrade) {
         pairEls.qr.innerHTML = '';
         pairState.lastRenderedUri = '';
         setCopyButtonDisabled(true, 'satellite-pair-ws-warning');
         return;
      }
      setCopyButtonDisabled(false);
      const uri = buildPairUri(serverUrl, pairState.key);
      if (uri === pairState.lastRenderedUri) return; /* no change → skip work */
      renderQrInto(pairEls.qr, uri);
      pairState.lastRenderedUri = uri;
   }

   function resetIdleTimer() {
      if (pairState.idleTimer) clearTimeout(pairState.idleTimer);
      pairState.idleTimer = setTimeout(closePairModal, PAIR_IDLE_TIMEOUT_MS);
   }

   function scheduleRevealAutoHide() {
      if (pairState.revealAutoHideTimer) clearTimeout(pairState.revealAutoHideTimer);
      pairState.revealAutoHideTimer = setTimeout(() => {
         if (pairState.revealed) setRevealed(false);
         pairState.revealAutoHideTimer = null;
      }, PAIR_REVEAL_AUTO_HIDE_MS);
   }

   function setRevealed(revealed) {
      pairState.revealed = revealed;
      if (!pairEls || !pairEls.keyInput || !pairEls.keyReveal) return;
      pairEls.keyInput.type = revealed ? 'text' : 'password';
      pairEls.keyReveal.textContent = revealed ? 'Hide' : 'Show';
      pairEls.keyReveal.setAttribute('aria-pressed', revealed ? 'true' : 'false');
      pairEls.keyReveal.setAttribute(
         'aria-label',
         revealed ? 'Hide registration key' : 'Show registration key'
      );
      /* Auto re-mask after 30s so a forgotten-open modal doesn't sit with the
       * key exposed in plain text in the DOM.  Idle auto-close (5min) is a
       * coarser backstop. */
      if (revealed) {
         scheduleRevealAutoHide();
      } else if (pairState.revealAutoHideTimer) {
         clearTimeout(pairState.revealAutoHideTimer);
         pairState.revealAutoHideTimer = null;
      }
   }

   function setCopyStatus(text, isError) {
      if (!pairEls || !pairEls.copyStatus) return;
      pairEls.copyStatus.textContent = text || '';
      pairEls.copyStatus.classList.toggle('satellite-pair-copy-status-error', !!isError);
   }

   function requestRegistrationKey() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({ type: 'get_satellite_registration_key' });
   }

   /* Tear down every timer the modal owns.  Called from closePairModal and any
    * future caller that needs to abort timers (e.g. WS disconnect mid-modal).
    * Keep this list complete — adding a new timer to pairState without listing
    * it here will leak it on close. */
   function clearPairTimers() {
      const fields = ['idleTimer', 'rerenderTimer', 'clipboardClearTimer', 'revealAutoHideTimer'];
      for (const f of fields) {
         if (pairState[f]) {
            clearTimeout(pairState[f]);
            pairState[f] = null;
         }
      }
   }

   function openPairModal() {
      if (!pairEls || !pairEls.modal) return;
      pairState.previousFocus = document.activeElement;
      pairState.revealed = false;
      pairState.key = '';
      pairState.tlsEnabled = false;
      pairState.lastRenderedUri = '';
      pairEls.keyInput.value = '';
      pairEls.server.value = defaultServerUrl();
      setRevealed(false);
      setCopyStatus('');
      pairEls.qr.innerHTML = '';
      pairEls.empty.classList.add('hidden');
      pairEls.qrWrap.classList.remove('hidden');
      setCopyButtonDisabled(true);
      pairEls.wsWarning.classList.add('hidden');
      pairEls.modal.classList.remove('hidden');
      if (callbacks.trapFocus) {
         pairState.focusCleanup = callbacks.trapFocus(pairEls.modal);
      }
      /* trapFocus puts focus on the first focusable element (the × button).
       * The Copy URI button is the user's likely next action and has clearer
       * semantics when announced first — override after trapFocus runs. */
      if (pairEls.copyBtn) pairEls.copyBtn.focus();
      pairState.keydownCleanup = wireEscapeToClose();
      resetIdleTimer();
      requestRegistrationKey();
   }

   function closePairModal() {
      clearPairTimers();
      if (pairState.focusCleanup) {
         pairState.focusCleanup();
         pairState.focusCleanup = null;
      }
      if (pairState.keydownCleanup) {
         pairState.keydownCleanup();
         pairState.keydownCleanup = null;
      }
      /* JS strings are immutable; we can't actually scrub the original heap
       * allocation.  Dropping the reference lets V8 GC the string when it next
       * runs.  Setting to '' here is the most we can do — it does NOT erase
       * the prior in-memory copy. */
      pairState.key = '';
      pairState.tlsEnabled = false;
      pairState.revealed = false;
      pairState.lastRenderedUri = '';
      if (pairEls) {
         if (pairEls.keyInput) pairEls.keyInput.value = '';
         if (pairEls.qr) pairEls.qr.innerHTML = '';
         if (pairEls.modal) pairEls.modal.classList.add('hidden');
      }
      if (pairState.previousFocus && pairState.previousFocus.focus) {
         try {
            pairState.previousFocus.focus();
         } catch (_) {
            /* node removed from DOM — ignore */
         }
      }
      pairState.previousFocus = null;
   }

   /* Escape close via the global DawnEscStack (LIFO — closes the topmost layer).
    * Returns a cleanup that unregisters; the caller stores it and invokes it on
    * every close path. */
   function wireEscapeToClose() {
      const token = DawnEscStack.register(() => {
         closePairModal();
         return true;
      });
      return () => DawnEscStack.unregister(token);
   }

   function handleRegistrationKeyResponse(payload) {
      if (!pairEls || pairEls.modal.classList.contains('hidden')) return;
      if (!payload) return;
      pairState.tlsEnabled = !!payload.tls_enabled;
      if (!payload.key_set || !payload.key) {
         /* Empty state — no key configured. */
         pairEls.qrWrap.classList.add('hidden');
         pairEls.empty.classList.remove('hidden');
         setCopyButtonDisabled(true);
         return;
      }
      pairState.key = payload.key;
      pairEls.keyInput.value = payload.key;
      rerenderPairModal();
   }

   async function onCopyUri() {
      if (!pairEls || !pairState.key) return;
      const serverUrl = (pairEls.server.value || '').trim();
      const isWs = /^ws:\/\//i.test(serverUrl);
      if (isWs && pairState.tlsEnabled) {
         setCopyStatus('Use wss:// when the server has TLS enabled.', true);
         return;
      }
      const uri = buildPairUri(serverUrl, pairState.key);
      try {
         await DawnFormat.copyToClipboard(uri);
         setCopyStatus('Copied. Local clipboard will clear in 30s.');
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('Pairing URI copied', 'success');
         }
         if (pairState.clipboardClearTimer) clearTimeout(pairState.clipboardClearTimer);
         pairState.clipboardClearTimer = setTimeout(async () => {
            try {
               await DawnFormat.copyToClipboard('');
               /* Only this device's clipboard is cleared; cross-device sync
                * (Universal Clipboard, KDE Connect, Gboard, etc.) may retain
                * the URI on other devices — there's no browser API to reach
                * those.  The modal disclaimer warns the user explicitly. */
               setCopyStatus('Local clipboard cleared.');
            } catch (_) {
               /* Best-effort — most failures here are benign. */
            }
            pairState.clipboardClearTimer = null;
         }, PAIR_CLIPBOARD_CLEAR_MS);
      } catch (_) {
         setCopyStatus('Copy failed — select the key manually.', true);
      }
   }

   function initPairModal() {
      pairEls = buildPairEls();
      if (!pairEls.modal || !pairEls.pairBtn) {
         pairEls = null;
         return;
      }
      pairEls.pairBtn.addEventListener('click', openPairModal);
      if (pairEls.closeBtn) pairEls.closeBtn.addEventListener('click', closePairModal);
      pairEls.modal.addEventListener('click', function (e) {
         /* Click on backdrop closes; click on modal content does not. */
         if (e.target === pairEls.modal) closePairModal();
      });
      if (pairEls.server) {
         pairEls.server.addEventListener('input', scheduleRerender);
      }
      if (pairEls.keyReveal) {
         pairEls.keyReveal.addEventListener('click', () => setRevealed(!pairState.revealed));
      }
      if (pairEls.copyBtn) {
         pairEls.copyBtn.addEventListener('click', onCopyUri);
      }
      /* Any activity inside the modal pushes the idle auto-close out. */
      ['mousemove', 'mousedown', 'keydown', 'touchstart'].forEach((evt) => {
         pairEls.modal.addEventListener(evt, resetIdleTimer);
      });
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
                  requestOtaList();
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
         refreshBtn.addEventListener('click', function () {
            requestListSatellites();
            requestOtaList();
         });
      }

      // Pair modal
      initPairModal();
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
      handleOtaListResponse: handleOtaListResponse,
      handleOtaPushResponse: handleOtaPushResponse,
      handleOtaPushAllResponse: handleOtaPushAllResponse,
      handleOtaRolloutStatusResponse: handleOtaRolloutStatusResponse,
      handleOtaRolloutAbortResponse: handleOtaRolloutAbortResponse,
      handleOtaError: handleOtaError,
      handleRegistrationKeyResponse: handleRegistrationKeyResponse,
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
         if (cbs && cbs.trapFocus) callbacks.trapFocus = cbs.trapFocus;
      },
   };
})();
