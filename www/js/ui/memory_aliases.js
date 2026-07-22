/**
 * DAWN Memory — entity-merge alias surface (Phase 1)
 *
 * Owns the alias panel + Suggested-Merges UI, the five alias-related
 * WebSocket request/response handlers, and the temporary merge-mode click
 * state.  Split out of memory.js on 2026-05-12 because that file exceeded
 * the 1500-line hard limit; Phase 2 auto-merge surfaces will land here.
 *
 * Loaded BEFORE memory.js in index.html so memory.js's init() can call
 * DawnMemoryAliases.init(ctx) once the parent module's state is ready.
 */
(function () {
   'use strict';

   /* =============================================================================
    * Module state — owned here, NOT shared with memory.js's memoryState.
    * ============================================================================= */

   /* Per-canonical alias rows keyed by entity id, populated by
    * handleAliasesResponse and consumed by renderAliasRowsFor on toggle. */
   const aliasesByEntity = {};

   /* Pending merge proposals for the current user (Suggested-Merges panel). */
   let proposals = [];

   /* Entity types where the "fuller" name form is the natural canonical
    * (manual-link direction-preference suggestion in handleMergeTargetClick).
    *
    * MIRROR: the C-side authoritative list lives in
    * src/memory/memory_db_alias_writes.c::entity_type_prefers_longer_canonical.
    * If THIS set changes, update the C function to match.  Drift is
    * caught by tests/check_swap_eligible_types_mirror.sh, wired into
    * ctest -L ci. */
   const SWAP_ELIGIBLE_TYPES = new Set(['person', 'pet', 'place']);

   /**
    * Decide whether the manual-link two-click flow should pre-flip
    * source/target before showing the confirm modal.
    *
    * Returns {swap: bool, reason: string|null}.  `reason` is set when
    * `swap` is true and identifies which rule triggered, so the modal can
    * show the right hint:
    *   - "mandatory-source-has-deps": source-as-clicked already has
    *     aliases pointing at it, so it can't be a valid soft-link source
    *     (alias_link refuses).  Swap is the only way to express the
    *     operator's intent; the leaf-target becomes the source.
    *   - "longer-canonical": both sides are leaves and the source has
    *     the fuller person/pet/place form (more tokens, or same-token-
    *     count with target as a strict prefix-substring).
    *
    * This is a SUPERSET of the C-side should_swap_for_longer_canonical
    * (memory_db_alias_writes.c).  The C cascade only handles inbound
    * entities that are freshly created (always 0 deps), so it never
    * needs the dep-asymmetry rule; the JS side meets entities at any
    * state in their lifecycle and must reason about deps explicitly.
    * The type-list MIRROR pin still applies — SWAP_ELIGIBLE_TYPES must
    * match entity_type_prefers_longer_canonical in C (drift caught by
    * tests/check_swap_eligible_types_mirror.sh).
    *
    * Never suggests demoting a user_self anchor — that requires moving
    * the is_user_self flag in lockstep, which manual-link doesn't do.
    * Filed as a follow-up "swap canonical" operation; until then, merges
    * involving a user_self land in the operator-clicked direction (and
    * fail server-side if the user_self has deps — operator sees the
    * standard error toast). */
   function shouldSwapForLongerCanonical(source, target) {
      if (!source || !target) return { swap: false, reason: null };
      if (!SWAP_ELIGIBLE_TYPES.has(source.entity_type)) return { swap: false, reason: null };
      if (!SWAP_ELIGIBLE_TYPES.has(target.entity_type)) return { swap: false, reason: null };
      if (source.is_user_self || target.is_user_self) return { swap: false, reason: null };

      const sourceHasDeps = (source.alias_count || 0) > 0;
      const targetHasDeps = (target.alias_count || 0) > 0;

      /* If target has deps, the swap would put a deps-laden entity on
       * the source side — alias_link's single-level-alias invariant
       * forbids that.  Don't suggest a swap that would create an
       * invalid link request, even if token count would otherwise
       * prefer it. */
      if (targetHasDeps) return { swap: false, reason: null };

      /* Source-as-clicked has deps and target is a leaf → swap is the
       * ONLY valid direction.  The operator's intent is "link these
       * two"; the only legal way is leaf-as-source, so flip.  Token
       * count doesn't matter — this rule outranks it because it's
       * about validity, not preference. */
      if (sourceHasDeps && !targetHasDeps) {
         return { swap: true, reason: 'mandatory-source-has-deps' };
      }

      /* Both sides are leaves — token-count heuristic decides. */
      const srcTokens = (source.canonical_name || '').trim().split(/\s+/).filter(Boolean).length;
      const tgtTokens = (target.canonical_name || '').trim().split(/\s+/).filter(Boolean).length;
      if (srcTokens > tgtTokens) return { swap: true, reason: 'longer-canonical' };
      /* Same token count: also swap if target's canonical is a strict
       * prefix-substring of source's (e.g. "Jon" vs "Jonathan"). */
      if (
         srcTokens === tgtTokens &&
         source.canonical_name &&
         target.canonical_name &&
         source.canonical_name.startsWith(target.canonical_name) &&
         source.canonical_name.length > target.canonical_name.length
      ) {
         return { swap: true, reason: 'longer-canonical' };
      }
      return { swap: false, reason: null };
   }

   /* When the operator clicks the merge button on a canonical, the next
    * entity click becomes the target.  null = not in merge mode. */
   let mergeSourceId = null;

   /* Injected at init() time — references to parent module state.  All reads
    * route through ctx so memory.js stays the source of truth for shared
    * state (memoryState.allEntities, memoryElements.list, etc.). */
   let ctx = null;

   /* =============================================================================
    * Init
    * ============================================================================= */

   function init(injectedCtx) {
      ctx = injectedCtx;
   }

   /* =============================================================================
    * API Requests — Phase 1 entity-merge alias surface
    * (handlers in webui_memory.c: handle_entity_aliases_request etc.)
    * ============================================================================= */

   function requestMergeEntities(sourceId, targetId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'merge_memory_entities',
         payload: { source_id: sourceId, target_id: targetId },
      });
   }

   function requestEntityAliases(entityId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_aliases_request',
         payload: { entity_id: entityId },
      });
   }

   function requestProposalList() {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_merge_proposal_list_request',
         payload: {},
      });
   }

   function requestEntityLink(sourceId, targetId, reason) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_link_request',
         payload: {
            source_entity_id: sourceId,
            target_entity_id: targetId,
            reason: reason || 'webui-operator',
         },
      });
   }

   function requestEntityUnlink(linkId) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_unlink_request',
         payload: { link_id: linkId, reason: 'split-by-operator' },
      });
   }

   function requestProposalResolve(proposalId, resolution) {
      if (typeof DawnWS === 'undefined' || !DawnWS.isConnected()) return;
      DawnWS.send({
         type: 'entity_proposal_resolve_request',
         payload: { proposal_id: proposalId, resolution: resolution },
      });
   }

   /* =============================================================================
    * WebSocket Response Handlers
    * ============================================================================= */

   function handleMergeResponse(payload) {
      if (!payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload.error || 'Failed to merge entities', 'error');
         }
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Entities merged', 'success');
      }
      if (ctx && typeof ctx.onEntitiesChanged === 'function') ctx.onEntitiesChanged();
   }

   function handleAliasesResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to load aliases', 'error');
         }
         return;
      }
      const entityId = payload.entity_id;
      aliasesByEntity[entityId] = payload.aliases || [];
      renderAliasRowsFor(entityId);
   }

   function handleProposalListResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Failed to load merge proposals', 'error');
         }
         return;
      }
      proposals = payload.proposals || [];
      /* The full-list response also implicitly carries the pending count;
       * keep the icon dot in sync without waiting for a separate push. */
      setProposalPendingCount(proposals.length);
      if (ctx && ctx.isEntitiesTabActive && ctx.isEntitiesTabActive()) {
         renderProposalsPanel();
      }
   }

   /* Server-pushed pending-proposal count.  Lights up the memory-icon dot
    * when count > 0 and clears it when zero.  Called from dawn.js's
    * `memory_proposals_changed` dispatch and from handleProposalListResponse
    * (which carries an implicit count via proposals.length).  The auto-
    * route-to-Graph-tab affordance reads from the same state — see
    * shouldAutoRouteToGraph() below. */
   let pendingProposalCount = 0;
   function setProposalPendingCount(count) {
      /* Number.isFinite rejects NaN AND Infinity (raw >= 0 lets +Inf
       * through, which would render "(Infinity suggested merges
       * pending)" if a buggy server push slipped in).  Negative or
       * non-numeric inputs also coerce to 0. */
      const n = Number.isFinite(count) && count >= 0 ? count : 0;
      pendingProposalCount = n;
      const btn = document.getElementById('memory-btn');
      if (btn) {
         btn.classList.toggle('has-proposals', n > 0);
         if (n > 0) {
            const noun = n === 1 ? 'merge' : 'merges';
            const tooltip = `Your Memory (${n} suggested ${noun} pending)`;
            const aria = `Your Memory — ${n} suggested ${noun} pending`;
            btn.setAttribute('title', tooltip);
            /* aria-label takes precedence over title for screen readers,
             * so static "View your memories" would leave SR users with
             * no signal that there's pending work.  Update both. */
            btn.setAttribute('aria-label', aria);
         } else {
            btn.setAttribute('title', 'Your Memory');
            btn.setAttribute('aria-label', 'View your memories');
         }
      }
   }

   function shouldAutoRouteToGraph() {
      return pendingProposalCount > 0;
   }

   function handleLinkResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Soft-link failed', 'error');
         }
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Soft alias created', 'success');
      }
      if (ctx && typeof ctx.onEntitiesChanged === 'function') ctx.onEntitiesChanged();
   }

   function handleUnlinkResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Split failed', 'error');
         }
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show('Alias split', 'success');
      }
      /* Reload entity list and proposals (split surfaces the source as a new
       * canonical).  Clear the per-entity alias cache so re-rendering shows
       * fresh state. */
      for (const k of Object.keys(aliasesByEntity)) delete aliasesByEntity[k];
      if (ctx && typeof ctx.onEntitiesChanged === 'function') ctx.onEntitiesChanged();
      if (ctx && ctx.isEntitiesTabActive && ctx.isEntitiesTabActive()) {
         requestProposalList();
      }
   }

   function handleProposalResolveResponse(payload) {
      if (!payload || !payload.success) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(payload?.error || 'Resolve failed', 'error');
         }
         return;
      }
      if (typeof DawnToast !== 'undefined') {
         const verb = payload.resolution === 'approved' ? 'approved' : 'rejected';
         DawnToast.show('Proposal ' + verb, 'success');
      }
      /* Refresh the proposals panel and the entity list (approve creates a
       * new alias).  Approving also adds a row to the target entity's
       * alias list — clear the per-entity cache so the next expand on
       * that target re-fetches the fresh state.  Matches the bulk-clear
       * pattern in handleUnlinkResponse above (we don't have the target
       * id in the response, so clear all). */
      requestProposalList();
      if (payload.resolution === 'approved') {
         for (const k of Object.keys(aliasesByEntity)) delete aliasesByEntity[k];
         if (ctx && typeof ctx.onEntitiesChanged === 'function') {
            ctx.onEntitiesChanged();
         }
      }
   }

   /* =============================================================================
    * Render — Suggested-Merges panel + per-canonical alias rows
    * ============================================================================= */

   /**
    * Composite-score band → CSS class name.  Defines the color of the
    * confidence badge on auto-merged aliases and pending proposals.  Bands
    * are coarse for legibility; numeric values follow MEMORY_ALIAS_AUTO /
    * REVIEW thresholds in include/memory/memory_db_aliases.h.
    */
   function compositeScoreBandClass(score) {
      if (score == null || score < 0) return 'alias-score-unknown';
      if (score >= 0.9) return 'alias-score-high';
      if (score >= 0.7) return 'alias-score-mid';
      return 'alias-score-low';
   }

   function renderProposalsPanelHtml() {
      if (proposals.length === 0) return '';

      const escapeHtml = ctx && ctx.escapeHtml ? ctx.escapeHtml : (s) => s;

      const itemsHtml = proposals
         .map((p) => {
            const scoreClass = compositeScoreBandClass(p.composite_score);
            const scorePct =
               p.composite_score != null && p.composite_score >= 0
                  ? Math.round(p.composite_score * 100) + '%'
                  : '?';
            return (
               `<div class="merge-proposal" data-proposal-id="${p.proposal_id}">` +
               `<div class="merge-proposal-body">` +
               `<span class="alias-confidence-badge ${scoreClass}" title="composite score">` +
               escapeHtml(scorePct) +
               '</span>' +
               `<span class="merge-proposal-pair">` +
               `<span class="merge-proposal-source">${escapeHtml(p.source_canonical_name || '?')}</span>` +
               ' <span class="entity-relation-arrow">→</span> ' +
               `<span class="merge-proposal-target">${escapeHtml(p.target_canonical_name || '?')}</span>` +
               '</span>' +
               '</div>' +
               '<div class="merge-proposal-actions">' +
               `<button class="btn-link merge-proposal-approve" ` +
               `data-proposal-id="${p.proposal_id}">Approve</button>` +
               `<button class="btn-link merge-proposal-reject" ` +
               `data-proposal-id="${p.proposal_id}">Reject</button>` +
               '</div>' +
               '</div>'
            );
         })
         .join('');

      return (
         '<div class="merge-proposals-panel">' +
         '<div class="merge-proposals-header">' +
         `<span>Suggested merges (${proposals.length})</span>` +
         '<span class="merge-proposals-hint">Approve to soft-link; reject to dismiss.</span>' +
         '</div>' +
         itemsHtml +
         '</div>'
      );
   }

   function renderProposalsPanel() {
      const list = ctx && ctx.memoryElements ? ctx.memoryElements.list : null;
      if (!list) return;
      /* Only re-render the panel in place to avoid disturbing the entity
       * scroll position. */
      const existing = list.querySelector('.merge-proposals-panel');
      const html = renderProposalsPanelHtml();
      if (existing) {
         if (html) {
            existing.outerHTML = html;
         } else {
            existing.remove();
         }
      } else if (html) {
         list.insertAdjacentHTML('afterbegin', html);
      }
   }

   /**
    * Render alias rows for a canonical entity, replacing any existing
    * placeholder.  Called after handleAliasesResponse populates
    * aliasesByEntity[entityId].
    */
   function renderAliasRowsFor(entityId) {
      const list = ctx && ctx.memoryElements ? ctx.memoryElements.list : null;
      if (!list) return;
      const card = list.querySelector(`.memory-item.entity[data-entity-id="${entityId}"]`);
      if (!card) return;
      const slot = card.querySelector('.entity-aliases');
      if (!slot) return;

      const escapeHtml = ctx && ctx.escapeHtml ? ctx.escapeHtml : (s) => s;
      const aliases = aliasesByEntity[entityId] || [];
      if (aliases.length === 0) {
         slot.innerHTML =
            '<div class="entity-aliases-empty">No aliases linked to this entity.</div>';
         return;
      }
      slot.innerHTML = aliases
         .map((a) => {
            const scoreClass = compositeScoreBandClass(a.composite_score);
            const scorePct =
               a.composite_score != null && a.composite_score >= 0
                  ? Math.round(a.composite_score * 100) + '%'
                  : '—';
            const linkKind = a.link_kind || 'soft';
            return (
               `<div class="entity-alias-row" data-link-id="${a.link_id}">` +
               `<span class="alias-confidence-badge ${scoreClass}" ` +
               `title="composite score / link kind: ${escapeHtml(linkKind)}">` +
               escapeHtml(scorePct) +
               '</span>' +
               `<span class="entity-alias-name">${escapeHtml(a.source_canonical_name || '?')}</span>` +
               `<button class="btn-link entity-alias-split" data-link-id="${a.link_id}" ` +
               `title="Split this alias back into a separate entity">Split</button>` +
               '</div>'
            );
         })
         .join('');
   }

   /* =============================================================================
    * Merge-mode interaction
    * ============================================================================= */

   /* Look up an entity by id across both the currently-rendered list
    * (`entities`, populated by both list and search responses) and the
    * paginated lookup table (`allEntities`, populated only by list
    * responses).  Search results show entities on screen without backfilling
    * allEntities, so a card the user can SEE may be absent from allEntities
    * — looking in both lists makes the merge flow robust against that. */
   function findEntityById(id) {
      if (!ctx || !ctx.memoryState) return null;
      const visible = ctx.memoryState.entities || [];
      const all = ctx.memoryState.allEntities || [];
      return visible.find((e) => e.id === id) || all.find((e) => e.id === id) || null;
   }

   function showMergeEntityPicker(sourceId) {
      const source = findEntityById(sourceId);
      if (!source) return;

      const knownCount =
         ctx && ctx.memoryState
            ? (ctx.memoryState.entities || []).length + (ctx.memoryState.allEntities || []).length
            : 0;
      if (knownCount < 2) {
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show('No other entities to merge with', 'info');
         }
         return;
      }

      /* If clicking the same merge button again, cancel merge mode (user-
       * initiated cancel — surface a toast). */
      if (mergeSourceId === sourceId) {
         cancelMergeMode(true);
         return;
      }

      /* Enter merge mode: highlight source, wait for target click. */
      mergeSourceId = sourceId;
      const list = ctx && ctx.memoryElements ? ctx.memoryElements.list : null;
      if (list) {
         const sourceEl = list.querySelector(`.memory-item.entity[data-entity-id="${sourceId}"]`);
         if (sourceEl) sourceEl.classList.add('merge-source');
         list.classList.add('merge-mode');
      }
      if (typeof DawnToast !== 'undefined') {
         DawnToast.show(
            `Select target entity to merge "${source.name}" into, or click merge again to cancel`,
            'info'
         );
      }
   }

   /* showToast=true is the user-initiated cancel (re-clicking the merge
    * button on the source).  Lifecycle callers (close / switchTab /
    * successful merge confirm) pass false so we don't toast on every panel
    * close. */
   function cancelMergeMode(showToast) {
      const wasActive = mergeSourceId !== null;
      const list = ctx && ctx.memoryElements ? ctx.memoryElements.list : null;
      if (list) {
         list.classList.remove('merge-mode');
         const sourceEl = list.querySelector('.merge-source');
         if (sourceEl) sourceEl.classList.remove('merge-source');
      }
      mergeSourceId = null;
      if (showToast && wasActive && typeof DawnToast !== 'undefined') {
         DawnToast.show('Merge canceled', 'info');
      }
   }

   function handleMergeTargetClick(targetId) {
      if (!mergeSourceId || mergeSourceId === targetId) return;

      const source = findEntityById(mergeSourceId);
      const target = findEntityById(targetId);
      if (!source || !target) {
         console.warn('DawnMemoryAliases: merge target lookup failed', {
            mergeSourceId,
            targetId,
            foundSource: !!source,
            foundTarget: !!target,
         });
         if (typeof DawnToast !== 'undefined') {
            DawnToast.show(
               'Could not resolve merge candidates — refresh the entity list and retry',
               'error'
            );
         }
         cancelMergeMode();
         return;
      }

      const sid = mergeSourceId;
      cancelMergeMode();

      /* Direction preference: see shouldSwapForLongerCanonical above for
       * the full rule.  Operator can still cancel the modal and re-click
       * the original direction if the suggestion is wrong. */
      let linkSrc = sid;
      let linkTgt = targetId;
      let linkSrcEnt = source;
      let linkTgtEnt = target;
      let swappedHint = '';
      const decision = shouldSwapForLongerCanonical(source, target);
      if (decision.swap) {
         linkSrc = targetId;
         linkTgt = sid;
         linkSrcEnt = target;
         linkTgtEnt = source;
         if (decision.reason === 'mandatory-source-has-deps') {
            swappedHint =
               ' Direction was flipped because "' +
               source.name +
               '" already has aliases attached — only the leaf entity can be a soft-link ' +
               'source. Cancel if you intended a different operation (e.g. split "' +
               source.name +
               '" first via the alias panel).';
         } else {
            swappedHint =
               ' Direction was flipped because "' +
               linkTgtEnt.name +
               '" is the more specific name — typically the better canonical. ' +
               'Cancel and re-click the other way if you intend the original direction.';
         }
      }

      /* Fire-and-forget (not awaited) so this handler stays synchronous — the
       * confirm was already non-blocking under the old callback API. */
      DawnDialog.confirm(`Soft-link "${linkSrcEnt.name}" → "${linkTgtEnt.name}"?`, {
         detail:
            `"${linkSrcEnt.name}" will be marked as a soft alias of "${linkTgtEnt.name}". ` +
            'Both rows are preserved and the link is reversible — you can split it ' +
            'later from the alias panel. Use "dawn-admin memory entity consolidate" ' +
            'to make a soft link permanent.' +
            swappedHint,
         danger: false,
         okText: 'Soft-link',
      })
         .then((ok) => {
            /* Phase 1 entity-merge: WebUI two-click defaults to soft alias. */
            if (ok) requestEntityLink(linkSrc, linkTgt, 'webui-operator');
         })
         .catch(() => {});
   }

   /**
    * Toggle a canonical entity's alias-expand panel.  Lazy-loads alias rows
    * on first expand by firing entity_aliases_request; reuses the cached
    * response on subsequent toggles.
    */
   function toggleAliasPanel(entityId, toggleBtn) {
      const list = ctx && ctx.memoryElements ? ctx.memoryElements.list : null;
      if (!list) return;
      const slot = list.querySelector(`[data-aliases-for="${entityId}"]`);
      if (!slot) return;
      const isHidden = slot.hasAttribute('hidden');
      if (isHidden) {
         slot.removeAttribute('hidden');
         if (toggleBtn) toggleBtn.setAttribute('aria-expanded', 'true');
         if (aliasesByEntity[entityId]) {
            renderAliasRowsFor(entityId);
         } else {
            slot.innerHTML = '<div class="entity-aliases-loading">Loading…</div>';
            requestEntityAliases(entityId);
         }
      } else {
         slot.setAttribute('hidden', '');
         if (toggleBtn) toggleBtn.setAttribute('aria-expanded', 'false');
      }
   }

   /* =============================================================================
    * Click dispatch — called from memory.js handleListClick.  Returns true if
    * the event was handled (so the parent stops further dispatch).
    * ============================================================================= */

   function tryHandleClick(e) {
      /* When merge mode is active, every click on an entity card that
       * isn't the source becomes the target — even on the card's own
       * buttons (merge / alias / contact / delete).  Otherwise users have
       * to hunt for a "neutral" area to click: the obvious affordances
       * (name, contact badge, merge icon) all sit on buttons that own
       * their own normal behaviors.  Clicking the SOURCE card's merge
       * button falls through to the cancel branch below. */
      if (mergeSourceId) {
         const entityEl = e.target.closest('.memory-item.entity');
         if (entityEl) {
            const targetId = parseInt(entityEl.dataset.entityId, 10);
            if (targetId && targetId !== mergeSourceId) {
               e.stopPropagation();
               handleMergeTargetClick(targetId);
               return true;
            }
            /* Clicked on the source card while in merge mode — fall
             * through; the merge-button branch below handles re-click
             * cancel. */
         }
      }

      /* Merge button — enter/cancel merge mode. */
      const mergeBtn = e.target.closest('.entity-merge-btn');
      if (mergeBtn) {
         e.stopPropagation();
         const sourceId = parseInt(mergeBtn.dataset.mergeEntityId, 10);
         showMergeEntityPicker(sourceId);
         return true;
      }

      /* Alias-badge toggle on canonical card. */
      const aliasToggle = e.target.closest('.entity-alias-badge');
      if (aliasToggle) {
         e.stopPropagation();
         const entityId = parseInt(aliasToggle.dataset.aliasToggleId, 10);
         toggleAliasPanel(entityId, aliasToggle);
         return true;
      }

      /* Split button on individual alias row. */
      const splitBtn = e.target.closest('.entity-alias-split');
      if (splitBtn) {
         e.stopPropagation();
         const linkId = parseInt(splitBtn.dataset.linkId, 10);
         /* Fire-and-forget so tryHandleClick stays synchronous (its boolean
          * return is consumed by the caller). */
         DawnDialog.confirm('Split this alias?', {
            detail:
               'The alias will be unlinked and surface again as its own entity. ' +
               'You can re-link later via the merge button.',
            danger: false,
            okText: 'Split',
         })
            .then((ok) => {
               if (ok) requestEntityUnlink(linkId);
            })
            .catch(() => {});
         return true;
      }

      /* Proposal approve / reject. */
      const approveBtn = e.target.closest('.merge-proposal-approve');
      if (approveBtn) {
         e.stopPropagation();
         const proposalId = parseInt(approveBtn.dataset.proposalId, 10);
         requestProposalResolve(proposalId, 'approved');
         return true;
      }
      const rejectBtn = e.target.closest('.merge-proposal-reject');
      if (rejectBtn) {
         e.stopPropagation();
         const proposalId = parseInt(rejectBtn.dataset.proposalId, 10);
         requestProposalResolve(proposalId, 'rejected');
         return true;
      }

      return false;
   }

   /* =============================================================================
    * Public surface
    * ============================================================================= */

   window.DawnMemoryAliases = {
      init,
      tryHandleClick,
      /* WebSocket response handlers (dispatched from dawn.js via the
       * DawnMemory.handle*Response thin-forwarders in memory.js). */
      handleMergeResponse,
      handleAliasesResponse,
      handleProposalListResponse,
      handleLinkResponse,
      handleUnlinkResponse,
      handleProposalResolveResponse,
      /* Lifecycle hooks called from memory.js. */
      renderProposalsPanel,
      requestProposalList,
      cancelMergeMode,
      /* Phase 2 proposal-pending indicator (memory-icon dot + auto-route
       * to Graph tab on open).  setProposalPendingCount is called from
       * dawn.js's memory_proposals_changed dispatch; shouldAutoRouteToGraph
       * is read by memory.js's open() to decide whether to switch tabs. */
      setProposalPendingCount,
      shouldAutoRouteToGraph,
   };
})();
