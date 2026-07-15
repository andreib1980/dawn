/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Phone call-audio settings panel.  Schema-driven controls that GET the current
 * config, and (debounced) push changes via set_config so they apply live to a
 * call in progress.  The server echoes back the stored (clamped) config.
 */
(function (global) {
   'use strict';

   const NS_OPTIONS = [
      { value: 'off', label: 'Off' },
      { value: 'low', label: 'Low' },
      { value: 'moderate', label: 'Moderate' },
      { value: 'high', label: 'High' },
      { value: 'veryhigh', label: 'Very high' },
   ];

   // Shared per-direction APM knobs (uplink adds echo_cancel).
   const APM_FIELDS = [
      {
         key: 'agc',
         type: 'toggle',
         label: 'Auto gain (AGC)',
         hint: 'Per-frame VAD-gated digital gain — the core fix for level pumping.',
      },
      {
         key: 'fixed_gain_db',
         type: 'number',
         label: 'Fixed gain (dB)',
         min: 0,
         max: 49,
         step: 0.5,
         hint: 'Static pre-gain, 0–49 (a value ≥ 50 disables AGC).',
      },
      {
         key: 'agc_ramp_db_per_s',
         type: 'number',
         label: 'AGC ramp (dB/s)',
         min: 0.1,
         max: 100,
         step: 0.5,
         hint: 'Lower = slower ramp = less pumping.',
      },
      {
         key: 'max_output_noise_dbfs',
         type: 'number',
         label: 'Noise floor (dBFS)',
         min: -90,
         max: 0,
         step: 1,
         hint: 'AGC will not lift the noise floor above this.',
      },
      {
         key: 'ns_level',
         type: 'select',
         label: 'Noise suppression',
         options: NS_OPTIONS,
         hint: 'If it sounds “underwater”, lower this.',
      },
      {
         key: 'high_pass',
         type: 'toggle',
         label: 'High-pass filter',
         hint: 'Removes DC / handling rumble.',
      },
   ];

   let els = { section: null, controls: null, status: null };
   let pcmPortInput = null; // modem PCM device-path text field (rebuilt each render)
   let webrtcAvailable = true;
   let suppressSend = false; // true while populating from a server response
   let saveTimer = null;
   let pendingUserSave = false; // a user-initiated save is awaiting its echo
   let appliedTimer = null;
   let lastBridgeActive = false;

   function send(msg) {
      if (typeof DawnWS !== 'undefined') {
         DawnWS.send(msg);
      }
   }

   function el(tag, attrs, children) {
      const e = document.createElement(tag);
      if (attrs) {
         Object.keys(attrs).forEach(function (k) {
            if (k === 'text') {
               e.textContent = attrs[k];
            } else if (k === 'class') {
               e.className = attrs[k];
            } else {
               e.setAttribute(k, attrs[k]);
            }
         });
      }
      (children || []).forEach(function (c) {
         e.appendChild(c);
      });
      return e;
   }

   // Build one control row; returns { row, input }.
   function buildField(idPrefix, field) {
      const id = idPrefix + '-' + field.key;
      let input;
      if (field.type === 'toggle') {
         input = el('input', { type: 'checkbox', id: id, class: 'dawn-toggle' });
      } else if (field.type === 'select') {
         input = el('select', { id: id, class: 'phone-audio-select' });
         field.options.forEach(function (opt) {
            const value = typeof opt === 'object' ? opt.value : opt;
            const label = typeof opt === 'object' ? opt.label : opt;
            input.appendChild(el('option', { value: value, text: label }));
         });
      } else {
         input = el('input', {
            type: 'number',
            id: id,
            class: 'phone-audio-number',
            min: field.min,
            max: field.max,
            step: field.step,
         });
      }
      input.setAttribute('aria-describedby', id + '-hint');
      input.addEventListener('change', scheduleSave);

      const label = el('label', { class: 'phone-audio-label', for: id, text: field.label });
      const hint = el('div', { class: 'phone-audio-hint', id: id + '-hint', text: field.hint });
      const controlWrap = el('div', { class: 'phone-audio-control' }, [input]);
      const row = el('div', { class: 'phone-audio-row' }, [label, controlWrap]);
      const group = el('div', { class: 'phone-audio-field' }, [row, hint]);
      return { group: group, input: input, field: field };
   }

   // Registry of all inputs so save/populate can iterate: { 'uplink.agc': input, ... }
   const inputs = {};

   function buildGroup(dirKey, title, fields, extras) {
      const body = el('div', { class: 'phone-audio-group-body' });
      fields.concat(extras || []).forEach(function (field) {
         const built = buildField(dirKey, field);
         inputs[dirKey + '.' + field.key] = built.input;
         body.appendChild(built.group);
      });
      const header = el('h4', { class: 'phone-audio-group-title', text: title });
      return el('div', { class: 'phone-audio-group' }, [header, body]);
   }

   // Build the "Device" group: a single free-text field for the modem PCM port.
   // Not an APM knob, so it lives outside the inputs registry and is populated /
   // collected explicitly.
   function buildDeviceGroup() {
      const input = el('input', {
         type: 'text',
         id: 'phone-pcm-port',
         class: 'phone-audio-text',
         spellcheck: 'false',
         autocapitalize: 'none',
         autocomplete: 'off',
         'aria-describedby': 'phone-pcm-port-hint',
      });
      input.addEventListener('change', scheduleSave);
      pcmPortInput = input;

      const label = el('label', {
         class: 'phone-audio-label',
         for: 'phone-pcm-port',
         text: 'Modem PCM port',
      });
      const hint = el('div', {
         class: 'phone-audio-hint',
         id: 'phone-pcm-port-hint',
         text: 'Modem USB audio node — prefer a stable /dev/serial/by-id/…-if06-port0 alias over a raw /dev/ttyUSBn. Blank uses the built-in default. Applies to the next call, not one in progress.',
      });
      const control = el('div', { class: 'phone-audio-control' }, [input]);
      const row = el('div', { class: 'phone-audio-row' }, [label, control]);
      const field = el('div', { class: 'phone-audio-field' }, [row, hint]);
      const header = el('h4', { class: 'phone-audio-group-title', text: 'Device' });
      return el('div', { class: 'phone-audio-group' }, [header, field]);
   }

   function render() {
      if (!els.controls) {
         return;
      }
      els.controls.textContent = '';
      pcmPortInput = null;
      Object.keys(inputs).forEach(function (k) {
         delete inputs[k];
      });

      // Device (PCM port) group first — it's the "which hardware" setting.
      els.controls.appendChild(buildDeviceGroup());

      // Uplink group.
      els.controls.appendChild(
         buildGroup('uplink', 'Your voice (uplink → far end)', APM_FIELDS, [
            {
               key: 'echo_cancel',
               type: 'toggle',
               label: 'Echo cancel (AECM)',
               hint: 'Enable only if the far end hears themselves. May need delay tuning.',
            },
         ])
      );

      // Downlink group: the "use APM" master toggle is the AGC switch, so drop the
      // inner per-APM `agc` field (it would be a confusing second AGC toggle).
      // Drop the inner AGC toggle, and give downlink fixed_gain its own hint (the
      // "≥50 disables AGC" caveat is uplink-specific — downlink AGC is the master
      // toggle, not this field's value).
      const downlinkKnobs = APM_FIELDS.filter(function (f) {
         return f.key !== 'agc';
      }).map(function (f) {
         if (f.key === 'fixed_gain_db') {
            return Object.assign({}, f, { hint: 'Static pre-gain, dB (0–49).' });
         }
         return f;
      });
      const dlGroup = buildGroup(
         'downlink',
         'Caller’s voice (downlink → your speaker)',
         [
            {
               key: 'apm',
               type: 'toggle',
               label: 'Use auto gain (AGC)',
               hint: 'Normalizes quiet vs. loud callers. Off = a simple soft-limiter (gain below).',
            },
         ].concat(downlinkKnobs),
         [
            {
               key: 'gain',
               type: 'number',
               label: 'Soft-limiter gain',
               min: 1,
               max: 32,
               step: 0.5,
               hint: 'Used only when auto gain is off.',
            },
         ]
      );
      els.controls.appendChild(dlGroup);
   }

   function setStatus(text, warn) {
      if (!els.status) {
         return;
      }
      els.status.textContent = text;
      els.status.classList.toggle('phone-audio-status-warn', !!warn);
   }

   // ---- server round-trip ----

   function requestConfig() {
      send({ type: 'get_phone_audio_config' });
   }

   function apmFromPayload(dirKey, obj) {
      if (!obj) {
         return;
      }
      APM_FIELDS.forEach(function (f) {
         const input = inputs[dirKey + '.' + f.key];
         if (!input || !(f.key in obj)) {
            return;
         }
         if (f.type === 'toggle') {
            input.checked = !!obj[f.key];
         } else {
            input.value = obj[f.key];
         }
      });
      if (dirKey === 'uplink' && inputs['uplink.echo_cancel'] && 'echo_cancel' in obj) {
         inputs['uplink.echo_cancel'].checked = !!obj.echo_cancel;
      }
   }

   function handleConfigResponse(payload) {
      if (!payload) {
         return;
      }
      suppressSend = true;
      webrtcAvailable = payload.webrtc_available !== false;

      if (pcmPortInput && payload.pcm_port_default) {
         pcmPortInput.placeholder = payload.pcm_port_default;
      }

      // A rejected save (e.g. a bad PCM port) echoes the stored config plus an
      // error. KEEP the user's typed text (don't revert) so they can fix a typo
      // in a long device path, mark the field invalid, and surface the message.
      if (payload.error) {
         if (pcmPortInput) {
            pcmPortInput.setAttribute('aria-invalid', 'true');
         }
         pendingUserSave = false;
         setStatus(payload.error, true);
         suppressSend = false;
         return;
      }

      // Accepted: reflect the stored value and clear any prior invalid state.
      // Don't clobber a value the user is mid-editing — a long path only commits
      // on blur, so a debounced echo from another control could otherwise wipe it.
      if (pcmPortInput) {
         if (document.activeElement !== pcmPortInput) {
            pcmPortInput.value = payload.pcm_port || '';
         }
         pcmPortInput.removeAttribute('aria-invalid');
      }

      apmFromPayload('uplink', payload.uplink);
      apmFromPayload('downlink', payload.downlink);
      if (inputs['downlink.apm']) {
         inputs['downlink.apm'].checked = !!payload.downlink_apm;
      }
      if (inputs['downlink.gain']) {
         // 0 = "use the bridge default"; show the effective default (8) so the
         // field isn't below its own min.
         const g = payload.downlink_gain;
         inputs['downlink.gain'].value = g && g > 0 ? g : 8;
      }

      applyEnabledState();

      lastBridgeActive = !!payload.bridge_active;
      if (pendingUserSave) {
         // This response is the echo of a user save — confirm it briefly.
         pendingUserSave = false;
         setStatus('Applied ✓', false);
         if (appliedTimer) {
            clearTimeout(appliedTimer);
         }
         appliedTimer = setTimeout(showRestingStatus, 1500);
      } else {
         showRestingStatus();
      }
      suppressSend = false;
   }

   function showRestingStatus() {
      if (!webrtcAvailable) {
         setStatus('WebRTC audio processing not built — only the soft-limiter gain is used.', true);
      } else if (lastBridgeActive) {
         // Audio knobs apply live; the PCM port only binds at the next call start,
         // so word this so it doesn't contradict the port field's own hint.
         setStatus(
            'Call in progress — audio changes apply live (PCM port on the next call).',
            false
         );
      } else {
         setStatus('No active call — changes apply to the next call.', false);
      }
   }

   // Reflect which controls actually do something: all APM controls are inert
   // without WebRTC; on the downlink the "use AGC" toggle decides whether the APM
   // knobs or the soft-limiter gain apply.
   function applyEnabledState() {
      if (!webrtcAvailable) {
         Object.keys(inputs).forEach(function (k) {
            inputs[k].disabled = k !== 'downlink.gain'; // only the soft-limiter works
         });
         return;
      }
      Object.keys(inputs).forEach(function (k) {
         inputs[k].disabled = false;
      });
      const dlApmOn = inputs['downlink.apm'] && inputs['downlink.apm'].checked;
      [
         'fixed_gain_db',
         'agc_ramp_db_per_s',
         'max_output_noise_dbfs',
         'ns_level',
         'high_pass',
      ].forEach(function (key) {
         const inp = inputs['downlink.' + key];
         if (inp) {
            inp.disabled = !dlApmOn;
         }
      });
      if (inputs['downlink.gain']) {
         inputs['downlink.gain'].disabled = dlApmOn;
      }
   }

   function collect() {
      function apmObj(dirKey, includeEcho) {
         const o = {};
         APM_FIELDS.forEach(function (f) {
            const input = inputs[dirKey + '.' + f.key];
            if (!input) {
               return;
            }
            if (f.type === 'toggle') {
               o[f.key] = input.checked;
            } else if (f.type === 'number') {
               const n = parseFloat(input.value);
               if (Number.isFinite(n)) {
                  o[f.key] = n; // skip a cleared/non-finite field rather than sending NaN -> 0
               }
            } else {
               o[f.key] = input.value;
            }
         });
         if (includeEcho && inputs[dirKey + '.echo_cancel']) {
            o.echo_cancel = inputs[dirKey + '.echo_cancel'].checked;
         }
         return o;
      }
      const out = {
         uplink: apmObj('uplink', true),
         downlink: apmObj('downlink', false),
         downlink_apm: inputs['downlink.apm'] ? inputs['downlink.apm'].checked : true,
      };
      if (inputs['downlink.gain']) {
         const g = parseFloat(inputs['downlink.gain'].value);
         if (Number.isFinite(g)) {
            out.downlink_gain = g;
         }
      }
      // Always send pcm_port (empty string clears the override). The field is
      // populated from the server on load, so it reflects the stored value.
      if (pcmPortInput) {
         out.pcm_port = pcmPortInput.value.trim();
      }
      return out;
   }

   function scheduleSave() {
      restoreFocusIfDisabled();
      if (suppressSend) {
         return;
      }
      if (saveTimer) {
         clearTimeout(saveTimer);
      }
      saveTimer = setTimeout(function () {
         pendingUserSave = true;
         send({ type: 'set_config', payload: { phone: collect() } });
      }, 400); // debounce so dragging a value doesn't spam the daemon
   }

   // If a control just disabled itself (e.g. toggling downlink AGC), move focus to
   // the toggle that caused it so focus doesn't drop to <body>.
   function restoreFocusIfDisabled() {
      applyEnabledState();
      const active = document.activeElement;
      if (active && active.disabled && inputs['downlink.apm'] && !inputs['downlink.apm'].disabled) {
         inputs['downlink.apm'].focus();
      }
   }

   function setElements(e) {
      els = Object.assign(els, e || {});
      render();
   }

   global.DawnPhoneAudio = {
      setElements: setElements,
      requestConfig: requestConfig,
      handleConfigResponse: handleConfigResponse,
   };
})(window);
