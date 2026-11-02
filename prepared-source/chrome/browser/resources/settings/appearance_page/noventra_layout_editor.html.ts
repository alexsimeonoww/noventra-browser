// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {NoventraLayoutEditorElement} from './noventra_layout_editor.js';
export function getHtml(this: NoventraLayoutEditorElement) {
  return html`
    <div class="editor-heading"><div><h2>Layout</h2>
      <p class="muted">Changes save immediately. Choose a starting layout or arrange individual controls.</p></div>
      <span class="draft-badge" ?data-dirty="${this.dirty()}">${this.savingStatus()}</span>
    </div>
    ${!this.draft ? html`<p role="status">${this.message || 'Loading your layout…'}</p>` : html`
      ${this.managed() ? html`<p class="notice">Your organization controls this layout.</p>` : ''}
      <div class="presets" aria-label="Layout presets">
        ${this.presetNames().map(name => html`<button data-preset="${name}" ?disabled="${this.controlsDisabled()}"
            aria-pressed="${this.presetMatches(name)}" title="${this.presetDescription(name)}"
            aria-label="${this.presetLabel(name)}. ${this.presetDescription(name)}" @click="${this.onPresetClick}">
          <span class="preset-preview" data-layout="${name}" aria-hidden="true"><span></span><span></span></span>
          <span class="preset-name">${this.presetLabel(name)}</span><span class="preset-description">${this.presetDescription(name)}</span>
        </button>`)}
      </div>
      <p class="preset-summary muted">${this.currentPresetDescription()}</p>
      <div class="position-control" style="margin-bottom:8px">
        <div class="position-label">Navigation controls</div>
        <div class="position-options" role="group" aria-label="Navigation controls" aria-describedby="navigationHint">
          ${this.navigationChoices().map(placement => html`<button data-navigation-placement="${placement}"
              ?disabled="${!this.canPlaceNavigation(placement)}" @click="${this.onNavigationClick}"
              aria-pressed="${this.navigationMatches(placement)}">${placement === 'top' ? 'Top' : 'With sidebar'}</button>`)}
        </div>
      </div>
      <p class="muted" id="navigationHint" style="margin-bottom:12px">${this.navigationHint()}</p>
      <div class="position-controls">
        ${[{id: 'tabs', label: 'Tabs position'}, {id: 'address', label: 'Address bar position'}].map(control => html`
          <div class="position-control">
            <div class="position-label">${control.label}${!this.positionItem(control.id)?.visible ? html`<span class="position-hidden">Hidden</span>` : ''}</div>
            <div class="position-options" role="group" aria-label="${control.label}" aria-describedby="positionHint">
              ${this.edges().map(edge => html`<button data-position-control="${control.id}" data-position-edge="${edge}"
                  ?disabled="${!this.canPlaceControl(control.id, edge)}" @click="${this.onPositionClick}"
                  aria-pressed="${this.positionItem(control.id)?.visible === true && this.positionItem(control.id)?.edge === edge}"
                  aria-label="Place ${this.label(control.id)} on ${this.edgeLabel(edge)}">${({top:'Top',left:'Left',right:'Right'})[edge]}</button>`)}
            </div>
          </div>`)}
        <p class="position-hint" id="positionHint">These choices move only the selected control. Use Navigation controls to move the toolbar together. Every choice saves automatically.</p>
      </div>
      <details id="individualControls" ?open="${this.searchMode || this.filter.length > 0}">
      <summary>Arrange individual controls</summary>
      <div class="preview-heading"><div><h3>Your browser layout</h3><p class="muted" id="previewHelp">Click a control to select it. Drag it to move.</p></div><span class="preview-mode">${this.applying || this.dirty() ? 'Saving changes…' : 'Current layout'}</span></div>
      <div class="preview-frame">
      <div class="canvas" aria-label="Browser layout preview" aria-describedby="previewHelp" ?data-dragging="${!!this.dragGhost}"
          ?data-comfortable="${this.tabDensity === 1}"
          style="--left-size:${this.previewSideSize('left')};--right-size:${this.previewSideSize('right')}">
        ${this.edges().map(edge => html`
          <section class="zone" data-edge="${edge}" aria-label="${this.edgeLabel(edge)}"
              ?data-populated="${this.items(edge).length > 0}" ?data-over="${this.dropTarget?.edge === edge}">
            ${this.selectedCapability()?.edges.includes(edge) ? html`<button class="edge-target" data-place-edge="${edge}" @click="${this.onEdgeClick}"
                ?disabled="${!this.canPlace(edge)}" aria-pressed="${this.selectedItem()?.visible === true && this.selectedItem()?.edge === edge}"
                title="Place ${this.label(this.selected)} on the ${edge}">${this.edgeLabel(edge)}</button>` : ''}
            ${this.previewGroups(edge).map(group => html`<div class="chips" ?data-tabs-group="${group.tabs}"
                ?data-sidebar-address="${!!group.sidebarAddress}">
              ${group.pinned ? html`<div class="preview-pinned" aria-hidden="true"><span><span class="tab-mark"></span></span><span><span class="tab-mark"></span></span><span><span class="tab-mark"></span></span></div>` : ''}
              ${group.items.map(item => html`
              <div class="${item.id === 'address' && edge === 'top' ? 'address-slot' : 'control-slot'}"><button class="chip" data-control="${item.id}" ?data-region="${this.isRegion(item.id)}"
        aria-label="${this.label(item.id)}" aria-pressed="${this.selected === item.id}"
        draggable="false" @pointerdown="${this.onControlPointerdown}" ?disabled="${this.controlsDisabled()}"
        ?data-before="${this.dropTarget?.before === item.id}" ?data-dragging="${this.dragGhost?.id === item.id}"
        title="${this.label(item.id)} — drag to move or click for options"
        @click="${this.onSelectClick}" @keydown="${this.onChipKeydown}">
      ${item.id === 'tabs' ? html`
        <span class="preview-tab" ?data-active="${edge === 'top'}" aria-hidden="true"><span class="tab-mark"></span><span class="preview-tab-label">${edge === 'top' ? 'New tab' : 'Example page'}</span><span class="tab-close">×</span></span>
        <span class="preview-tab" ?data-active="${edge !== 'top'}" aria-hidden="true"><svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath('action:settings')}"></path></svg><span class="preview-tab-label">Settings</span><span class="tab-close">×</span></span>
      ` : html`${item.id === 'favorites' || item.id === 'bookmarks' ? html`
        <svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath(item.id)}"></path></svg>
        <span class="preview-library" aria-hidden="true"><span class="preview-library-title">${this.label(item.id) + ' ⌄'}</span>
          ${item.id === 'favorites' ? html`<span class="preview-library-tiles">${[1,2,3,4,5,6].map(() => html`<span class="preview-library-tile"><span class="tab-mark"></span></span>`)}</span><span class="preview-library-hint">Manage favorites</span>` : html`
            <span class="preview-library-row"><span>⌄</span><svg class="control-icon" viewBox="0 0 24 24"><path d="${this.iconPath('action:bookmarks')}"></path></svg><span>Bookmark folder</span></span>
            <span class="preview-library-row"><span class="tab-mark"></span><span>Saved page</span></span>
            <span class="preview-library-hint">All bookmarks</span>`}
        </span>
      ` : html`${item.id === 'address' ? html`
        <svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath('address')}"></path></svg><span class="address-text" aria-hidden="true">${edge === 'top' ? 'Search or enter an address' : 'example.com'}</span>
        <svg class="control-icon address-star" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath('saved-sites')}"></path></svg>
      ` : html`<svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath(item.id)}"></path></svg>`}`}`}
      <span class="control-label" ?data-new-tab-label="${item.id === 'new-tab' && edge !== 'top'}">${this.label(item.id)}</span>
    </button></div>`)}${group.last && this.dropTarget?.edge === edge && !this.dropTarget.before && this.dragGhost?.id !== 'spaces' ? html`<span class="drop-end" aria-hidden="true"></span>` : ''}</div>`)}
            ${(edge === 'left' || edge === 'right') && (this.footerItems(edge).length ||
                (this.dropTarget?.edge === edge && this.dragGhost?.id === 'spaces')) ? html`
              <div class="space-footer" ?data-menu="${this.footerItems(edge).some(item => item.id === 'menu')}">
                ${this.footerItems(edge).map(item => html`
                  <button class="chip" data-control="${item.id}" ?data-region="${this.isRegion(item.id)}"
                      aria-label="${this.label(item.id)}" aria-pressed="${this.selected === item.id}"
                      draggable="false" @pointerdown="${this.onControlPointerdown}" ?disabled="${this.controlsDisabled()}"
                      ?data-before="${this.dropTarget?.before === item.id}"
                      ?data-dragging="${this.dragGhost?.id === item.id}"
                      title="${this.label(item.id)} — drag to move or click for options"
                      @click="${this.onSelectClick}" @keydown="${this.onChipKeydown}">
                    ${item.id === 'spaces' ? html`<span class="preview-space-dots" aria-hidden="true">
                      <span class="preview-space-dot" data-active></span>
                      <span class="preview-space-dot"></span>
                      <span class="preview-space-dot"></span>
                    </span>${!this.footerItems(edge).some(item => item.id === 'menu') ? html`<span class="preview-space-more" aria-hidden="true">⋯</span>` : ''}` : html`<svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath(item.id)}"></path></svg>`}
                    <span class="control-label">${this.label(item.id)}</span>
                  </button>`)}
                ${this.dropTarget?.edge === edge && this.dragGhost?.id === 'spaces' ? html`<span class="drop-end" aria-hidden="true"></span>` : ''}
              </div>` : ''}
            ${!this.items(edge).length && this.canPlace(edge) ? html`<div class="empty-zone">Drop here</div>` : ''}
          </section>`)}
        <div class="page" aria-label="Example new tab page">
          ${this.commandPopupVisible() ? html`<div class="preview-command-popup" aria-hidden="true"><svg class="control-icon" viewBox="0 0 24 24"><path d="${this.iconPath('address')}"></path></svg><span>example.com</span><svg class="control-icon" viewBox="0 0 24 24"><path d="${this.iconPath('saved-sites')}"></path></svg></div>` : ''}
          <div class="preview-homebrand"><span class="page-mark" aria-hidden="true"></span><span>noventra</span></div>
          <div class="preview-search" aria-hidden="true"><svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath('address')}"></path></svg><span>Search or enter an address</span></div>
          <div class="preview-favorites" aria-hidden="true"><span class="preview-favorites-title">Favorite sites</span><div class="preview-shortcut"><span>+</span><span>Add shortcut</span></div></div>
          <svg class="preview-customize control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="m5 16 11-11 3 3-11 11-4 1zM14 7l3 3"></path></svg>
        </div>
      </div>
      ${this.selection().map(({item, cap}) => html`
        <section class="selection-tools" aria-label="Selected control">
          <p class="selection-eyebrow">Editing</p>
          <div class="selection-info">
            <div class="selection-title"><svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath(item.id)}"></path></svg><span class="selection-name">${this.label(item.id)}</span><span class="selection-position">${item.visible ? this.edgeLabel(item.edge) : 'Hidden from layout'}</span></div>
            <p class="muted">${this.controlDescription(item.id)}</p>
          </div>
          <div class="selection-actions">
            <div class="placement-options" role="group" aria-label="Choose a position">
              ${this.edges().filter(edge => cap.edges.includes(edge)).map(edge => html`<button data-quick-edge="${edge}" @click="${this.onQuickEdgeClick}"
                  ?disabled="${!this.canPlace(edge)}" aria-pressed="${item.visible && item.edge === edge}"
                  aria-label="Place ${this.label(item.id)} on ${this.edgeLabel(edge)}">${({top:'Top bar',left:'Left sidebar',right:'Right sidebar'})[edge]}</button>`)}
            </div>
            ${item.id !== 'spaces' ? html`<div class="reorder-options" role="group" aria-label="Order within this position">
              <button @click="${this.onMoveEarlierClick}" ?disabled="${!this.canReorder(-1)}" aria-label="Move ${this.label(item.id)} earlier">← Earlier</button>
              <button @click="${this.onMoveLaterClick}" ?disabled="${!this.canReorder(1)}" aria-label="Move ${this.label(item.id)} later">Later →</button>
            </div>` : ''}
            <button id="hideControl" ?disabled="${this.controlsDisabled() || !item.visible || !cap.hideable || cap.requiredVisibility === true}"
                @click="${this.onHideClick}">Hide control</button>
          </div>
          ${cap.requiredVisibility !== undefined ? html`<p class="restriction">Visibility is managed by your organization.</p>` : ''}
          ${cap.restriction ? html`<p class="restriction">${cap.restriction}</p>` : ''}
          ${cap.members?.length ? html`<p class="restriction">Installed: ${cap.members.map(member => member.label).join(', ')}</p>` : ''}
        </section>`)}
      </div>
      <p class="preview-note muted">Example tabs and page content. Layout changes keep your open tabs.</p>
      ${this.items('left').some(item => item.id === 'tabs') || this.items('right').some(item => item.id === 'tabs') ? html`
        <p class="preview-note muted">Sidebar presets hide the address row. Press Ctrl+L to edit in the centered address bar. New Tab follows the tabs. Bookmarks, workspace dots and Menu share one bottom row, and stack in the icon rail. Resize the sidebar from 144 to 320 pixels, or collapse it to a 48-pixel icon rail. Custom arrangements keep their control order.</p>` : ''}
      <section class="tray" aria-label="Available controls" ?data-over="${this.dropTarget?.edge === 'tray'}">
        <div class="tray-heading"><div><h3>Add controls <span class="count">${this.availableItems().length}</span></h3>
          <p class="muted">Select a control, then choose where it goes.</p></div>
          <input class="control-search" type="search" aria-label="Find a control" placeholder="Search controls…" .value="${this.filter}" @input="${this.onFilterInput}">
        </div>
        <div class="control-groups">${this.availableGroups().map(group => html`
          <section class="control-group" aria-label="${group.label}">
            <h4>${group.label}</h4>
            <div class="chips">${group.items.map(item => html`
    <button class="chip" data-control="${item.id}" ?data-region="${this.isRegion(item.id)}" aria-label="${this.label(item.id)}" aria-pressed="${this.selected === item.id}"
        draggable="false" @pointerdown="${this.onControlPointerdown}" ?disabled="${this.controlsDisabled()}"
        ?data-before="${this.dropTarget?.before === item.id}" ?data-dragging="${this.dragGhost?.id === item.id}"
        title="${this.label(item.id)} — drag into the preview or click to choose a position"
        @click="${this.onSelectClick}" @keydown="${this.onChipKeydown}">
      <span class="grip" aria-hidden="true">⠿</span><svg class="control-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="${this.iconPath(item.id)}"></path></svg><span class="control-label">${this.label(item.id)}</span>
    </button>`)}</div>
          </section>`)}</div>
        ${!this.availableItems().length ? html`<p class="muted">${this.filter.trim() ? 'No matching controls. Try another name.' : 'All controls are in your layout.'}</p>` : ''}
        <p class="muted tray-hint">Drop a control here to hide it. You can add it back at any time.</p>
      </section>
      <details class="keyboard-help"><summary>Keyboard shortcuts & tips</summary>
        <p class="muted">Select a control. Alt + arrows changes its order; Alt + Shift + Up, Left or Right moves it to that edge. Delete hides it. Ctrl + Z undoes; Ctrl + Shift + Z redoes. Escape cancels a drag.</p>
        <p class="muted">Ctrl + L always opens address entry. Alt + F opens the browser menu. Drag the sidebar's inner edge to resize it.</p>
      </details>
      </details>
      <div class="footer">
        <div class="footer-actions">
          <button id="undoLayout" ?disabled="${this.controlsDisabled() || !this.draft.undo.length}" @click="${this.onUndoClick}">Undo</button>
          <button id="redoLayout" ?disabled="${this.controlsDisabled() || !this.draft.redo.length}" @click="${this.onRedoClick}">Redo</button>
          <button id="resetLayout" ?disabled="${this.controlsDisabled()}" @click="${this.onResetClick}" title="Restore the default left sidebar. Appearance and Spaces are kept.">Reset layout</button>
          <span class="spacer"></span>
          ${this.conflict || this.saveFailed ? html`<button id="reloadLayout" ?disabled="${this.applying || this.reloading}" @click="${this.onReloadCurrentClick}">${this.reloading ? 'Loading…' : 'Reload current layout'}</button>` : ''}
          ${this.saveFailed && !this.conflict && this.dirty() ? html`<button id="retryLayoutSave" class="primary" ?disabled="${this.applying || this.managed()}" @click="${this.onRetrySaveClick}">Retry saving</button>` : ''}
        </div>
        <p class="status" role="status" aria-live="polite">${this.message || 'Changes save automatically.'}</p>
      </div>
    `}
    ${this.dragGhost ? html`<div class="drag-ghost" aria-hidden="true" style="left:${this.dragGhost.x}px;top:${this.dragGhost.y}px">
      <svg class="control-icon" viewBox="0 0 24 24"><path d="${this.iconPath(this.dragGhost.id)}"></path></svg>${this.label(this.dragGhost.id)}
    </div>` : ''}
    <dialog aria-labelledby="leaveTitle" @cancel="${this.onDialogCancel}">
      <h2 id="leaveTitle">Leave without saving?</h2>
      <p class="muted">Your latest layout choice could not be saved. Stay here to resolve the error, or leave using the last saved layout.</p>
      <div class="dialog-actions">
        <button id="keepEditing" autofocus @click="${this.onKeepEditingClick}">Stay here</button>
        <button id="discardChanges" @click="${this.onDiscardAndLeaveClick}">Leave without saving</button>
      </div>
    </dialog>`;
}
