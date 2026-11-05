// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
import {addWebUiListener, removeWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';
import type {CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {CrLitElement, css} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {acknowledgeSave, canPlaceNavigation, createDraft, dropBefore, EDGES, isDirty, keyboardEdge, navigationEdge, navigationMatches, reduceDraft, reconcileDraft, visibleItems, workspacePreset} from './noventra_layout_model.js';
import type {BrowserLayout, DockEdge, DraftAction, LayoutCapability, LayoutDraft, LayoutItem, NavigationPlacement} from './noventra_layout_model.js';
import {getHtml} from './noventra_layout_editor.html.js';
interface LayoutState {
  layout: BrowserLayout; capabilities: LayoutCapability[]; presets: Record<string, BrowserLayout>;
}
interface ApplyResult {status: string; message: string; layout: BrowserLayout;}
export class NoventraLayoutEditorElement extends CrLitElement {
  static get is() { return 'noventra-layout-editor'; }
  static override get styles(): CSSResultGroup {
    return css`
      :host {display:block;padding:24px;color:var(--cr-primary-text-color);--line:var(--cr-separator-color);--accent:var(--cr-link-color);--preview-surface:var(--color-new-tab-page-background,color-mix(in srgb,var(--cr-card-background-color) 96%,var(--cr-primary-text-color)));--preview-page-ink:contrast-color(var(--preview-surface));--preview-chrome:var(--color-toolbar,var(--cr-card-background-color));--preview-ink:var(--color-toolbar-text,var(--cr-primary-text-color));}
      * {box-sizing:border-box} h2 {font-size:20px;font-weight:500;margin:0 0 6px} h3 {font-size:13px;font-weight:500;margin:0}
      p {margin:0;line-height:1.6} .muted {color:var(--cr-secondary-text-color);font-size:12px}
      button {font:inherit;color:inherit;background:var(--cr-card-background-color);border:1px solid var(--line);border-radius:8px;padding:8px 10px;cursor:pointer;min-height:34px;transition:background-color 140ms ease,border-color 140ms ease,color 140ms ease}
      button:hover:enabled {background:var(--cr-hover-background-color)} button:focus-visible,input:focus-visible,summary:focus-visible {outline:2px solid var(--accent);outline-offset:2px}
      button:disabled {opacity:.45;cursor:default} .primary {background:var(--cr-button-background-color,var(--accent));color:contrast-color(var(--cr-button-background-color,var(--accent)));border-color:transparent}
      .primary:hover:enabled {background:var(--cr-button-background-color,var(--accent));filter:brightness(.95)}
      .editor-heading {display:flex;align-items:start;justify-content:space-between;gap:12px;flex-wrap:wrap}.draft-badge {border-radius:6px;padding:5px 9px;font-size:11px;white-space:nowrap;background:var(--cr-hover-background-color);color:var(--cr-secondary-text-color)}.draft-badge[data-dirty] {color:var(--accent);background:color-mix(in srgb,var(--accent) 10%,transparent)}
      .guide {display:flex;gap:16px;flex-wrap:wrap;padding:0;list-style:none;counter-reset:steps;margin:18px 0;color:var(--cr-secondary-text-color);font-size:12px}.guide li {counter-increment:steps;display:flex;align-items:center;gap:7px}.guide li::before {content:counter(steps);display:grid;place-items:center;width:20px;height:20px;border-radius:50%;background:var(--cr-hover-background-color);font-size:11px}
      .presets {display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px;margin:0 0 24px}.presets button {display:flex;flex-direction:column;gap:5px;align-items:start;text-align:start;min-width:0;padding:10px;font-size:12px}.presets button[aria-pressed=true] {border-color:color-mix(in srgb,var(--cr-primary-text-color) 55%,transparent);background:color-mix(in srgb,var(--cr-primary-text-color) 7%,transparent)}.preset-description {font-size:11px;color:var(--cr-secondary-text-color);line-height:1.4}.preset-name {font-weight:500}
      .preset-preview {width:48px;height:32px;background:var(--cr-hover-background-color);border:1px solid var(--line);border-radius:4px;position:relative;margin-bottom:3px}.preset-preview span:first-child {position:absolute;inset:3px auto 3px 3px;width:10px;border-radius:2px;background:var(--cr-primary-text-color);opacity:.3}.preset-preview span:last-child {position:absolute;inset:8px 5px auto 18px;height:2px;background:var(--line);border-radius:2px}.preset-preview[data-layout=minimal-right] span:first-child {inset:3px 3px 3px auto}.preset-preview[data-layout=minimal-right] span:last-child {inset:8px 18px auto 5px}.preset-preview[data-layout=classic-top] span:first-child {inset:3px 3px auto;width:auto;height:6px}
      .preset-preview[data-layout=classic-top] span:first-child {inset:3px 3px auto;width:auto;height:4px}
      .preset-preview[data-layout=classic-top] span:last-child {inset:11px 3px auto;width:auto;height:3px}
      .preset-preview[data-layout=compact-top] span:first-child {inset:3px 3px auto;width:auto;height:6px}
      .preset-preview[data-layout=compact-top] span:last-child {display:none}
      .preview-heading {display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap;margin-bottom:10px}
      .position-controls {display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin-bottom:18px}.position-control {min-width:0;padding:12px;border:1px solid var(--line);border-radius:8px}.position-label {display:flex;align-items:center;gap:6px;font-size:12px;margin-bottom:9px}.position-hidden {font-size:11px;color:var(--cr-secondary-text-color)}.position-options {display:flex;gap:4px}.position-options button {flex:1;min-width:0;padding:6px 7px;font-size:11px;min-height:32px}.position-options [aria-pressed=true] {border-color:var(--accent);color:var(--accent);background:color-mix(in srgb,var(--accent) 8%,var(--cr-card-background-color))}.position-hint {grid-column:1/-1;font-size:11px;color:var(--cr-secondary-text-color)}
      .library-options {display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin-bottom:18px}.library-option {display:flex;align-items:center;gap:10px;padding:12px;border:1px solid var(--line);border-radius:8px;min-width:0}.library-option>div {flex:1;min-width:0}.library-option p {font-size:11px;color:var(--cr-secondary-text-color);margin-top:4px}.library-option button {flex:none;font-size:11px;padding:6px 9px}.library-option [aria-pressed=true] {color:var(--accent);border-color:var(--accent)}
      .preview-library {display:none}.zone:is([data-edge=left],[data-edge=right]) .chip:is([data-control=favorites],[data-control=bookmarks]) {height:auto;padding:4px 3px;display:block}.zone:is([data-edge=left],[data-edge=right]) .chip:is([data-control=favorites],[data-control=bookmarks])>.control-icon {display:none}.zone:is([data-edge=left],[data-edge=right]) .preview-library {display:flex;flex-direction:column;align-items:stretch;gap:6px;min-width:0;width:100%;font-size:10px}.preview-library-title {text-align:start;font-size:9px;color:var(--cr-secondary-text-color)}.preview-library-tiles {display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:4px}.preview-library-tile {--preview-tab-surface:var(--color-toolbar-background-subtle-emphasis,var(--cr-hover-background-color));display:grid;place-items:center;border-radius:6px;height:28px;background:var(--color-toolbar-background-subtle-emphasis,var(--cr-hover-background-color))}.preview-library-row {display:flex;align-items:center;gap:5px;text-align:start;min-width:0;height:22px}.preview-library-row span:last-child {white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.preview-library-row .control-icon {width:12px;height:12px}.preview-library-hint {text-align:start;font-size:8px;opacity:.7}
      .preview-frame {border:1px solid var(--line);border-radius:10px;overflow:hidden}
      .preview-mode {font-size:11px;color:var(--cr-secondary-text-color);white-space:nowrap}
      .preview-heading h3 {margin-bottom:4px}.preview-note {margin-top:8px;font-size:11px}
      .canvas {--preview-divider:color-mix(in srgb,var(--preview-ink) 22%,transparent);container-type:inline-size;scroll-margin-block:80px 160px;overflow:hidden;display:grid;grid-template-columns:minmax(0,var(--left-size)) minmax(0,1fr) minmax(0,var(--right-size));grid-template-rows:min-content minmax(326px,1fr);background:var(--preview-chrome);color:var(--preview-ink);min-height:360px}
      :host([glass-enabled]) {--preview-surface:var(--preview-chrome)}
      .canvas .zone[data-edge=top][data-populated] {box-shadow:inset 0 -1px var(--preview-divider)}
      .canvas .zone[data-edge=left][data-populated] {box-shadow:inset -1px 0 var(--preview-divider)}
      .canvas .zone[data-edge=right][data-populated] {box-shadow:inset 1px 0 var(--preview-divider)}
      .zone {scroll-margin-block:116px 160px;min-width:0;min-height:16px;position:relative;border:1px dashed transparent;transition:background-color 140ms ease,border-color 140ms ease}
      .zone[data-populated] {padding:6px}.zone[data-edge=top] {grid-column:1/-1;grid-row:1}.zone[data-edge=left] {grid-column:1;grid-row:2}.zone[data-edge=right] {grid-column:3;grid-row:2}
      .zone:not([data-populated]) {min-height:0;border-width:0}
      .zone[data-edge=top][data-populated] {padding:0 6px;border-width:0}.zone[data-edge=top] .chips {row-gap:0}.zone[data-edge=top] .chip:not([data-control=tabs]) {margin-block:3px}
      .zone[data-edge=top] .chip:not([data-control=tabs]):not([data-control=address]) {width:28px;flex:0 0 28px}
      .canvas .chip[data-control=menu] .control-icon {stroke-width:3}
      .canvas .chip:focus-visible {outline-offset:-2px}
      .zone[data-over] {background:color-mix(in srgb,var(--accent) 12%,var(--preview-chrome));border-color:var(--accent)}
      .chips {display:flex;gap:6px;flex-wrap:wrap;align-items:start}.canvas .chips {gap:3px;align-items:center}.zone[data-edge=left] .chips,.zone[data-edge=right] .chips {display:grid;grid-template-columns:repeat(auto-fill,minmax(26px,1fr));align-content:start;gap:3px}
      .chip {scroll-margin-block:116px 160px;touch-action:none;user-select:none;font-size:11px;display:flex;gap:7px;align-items:center;text-align:start;max-width:100%;min-width:0;min-height:32px;padding:6px 8px;position:relative;cursor:grab}
      .chip .control-label {overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.grip {opacity:.4;font-size:12px;flex:none}
      .control-icon {width:16px;height:16px;flex:none;fill:none;stroke:currentColor;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}
      .canvas .chip {padding:5px;min-height:28px;height:28px;justify-content:center;background:transparent;border-color:transparent;border-radius:6px;color:var(--preview-ink);gap:5px}.canvas .chip:hover:enabled {background:var(--cr-hover-background-color)}.canvas .control-label {position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%)}
      .chip[aria-pressed=true] {border-color:var(--accent);background:color-mix(in srgb,var(--accent) 10%,transparent)}
      .canvas .chip[data-control=address] {justify-content:start;border:1px solid var(--preview-divider);border-radius:6px;min-width:0;width:100%;background:transparent}
      .canvas .chip[aria-pressed=true] {border-color:var(--accent);background:color-mix(in srgb,var(--accent) 8%,var(--preview-chrome))}
      .control-slot {display:contents}
      .address-slot {display:flex;align-items:center;justify-content:center;flex:1 1 120px;min-width:100px}
      .address-slot .chip[data-control=address] {flex:0 1 auto;max-width:min(720px,60cqw)}
      .address-text {font-size:10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;flex:1;text-align:center}.address-star {width:13px;height:13px;opacity:.65}
      .canvas .chip[data-control=tabs] {display:flex;align-items:stretch;justify-content:start;gap:4px;height:auto;padding:4px;flex-basis:100%;min-height:32px}.preview-tab {display:flex;align-items:center;gap:6px;min-width:0;flex:1;border-radius:6px;padding:6px;font-size:10px;text-align:start;line-height:14px}.preview-tab[data-active] {--preview-tab-surface:var(--color-toolbar-background-subtle-emphasis,color-mix(in srgb,var(--preview-chrome) 94%,var(--preview-ink)));background:var(--preview-tab-surface)}.preview-tab-label {white-space:nowrap;overflow:hidden;text-overflow:ellipsis;flex:1}.tab-close {font-size:13px;color:var(--cr-secondary-text-color)}.tab-mark {width:12px;height:12px;flex:none;mask:url(chrome://theme/IDR_PRODUCT_LOGO@2x) center/contain no-repeat;background:contrast-color(var(--preview-tab-surface,var(--preview-chrome)))}
      .zone[data-edge=left] .chip[data-region],.zone[data-edge=right] .chip[data-region] {grid-column:1/-1;min-width:0;width:100%}.zone[data-edge=left] .chip[data-control=tabs],.zone[data-edge=right] .chip[data-control=tabs] {flex-direction:column;align-items:stretch;min-height:126px;justify-content:start}.zone[data-edge=left] .preview-tab,.zone[data-edge=right] .preview-tab {flex:none}
      .zone:is([data-edge=left],[data-edge=right])>.chips {flex:none}
      .zone:is([data-edge=left],[data-edge=right])>.chips[data-tabs-group] {display:flex;flex-direction:column;align-items:stretch;flex:none;min-height:0}
      .zone:is([data-edge=left],[data-edge=right])>.chips[data-tabs-group] .chip[data-control=tabs] {flex:none;min-height:0}
      .zone:is([data-edge=left],[data-edge=right]) {display:flex;flex-direction:column}.space-footer {margin-top:auto;padding-top:6px;flex:none;container-type:inline-size}.canvas .chip[data-control=spaces] {justify-content:center;align-items:center;width:100%;height:30px;min-height:30px;padding:3px 5px;gap:12px}.preview-space-dots {display:flex;align-items:center;gap:12px}.preview-space-dot {flex:none;width:6px;height:6px;box-sizing:border-box;border:1.5px solid currentColor;border-radius:50%}.preview-space-dot[data-active] {width:8px;height:8px;background:currentColor}.preview-space-more {flex:none;width:12px;text-align:center;font-size:14px}.space-footer .drop-end {display:block}

      .zone[data-edge=top] .chip[data-control=tabs] {flex:2 1 160px;min-width:120px;height:34px;min-height:34px;padding:2px;gap:4px}.zone[data-edge=top] .preview-tab {flex:1 1 120px;padding:5px 7px}

      .chip[data-before]::before {content:'';position:absolute;inset:-3px auto -3px -3px;width:2px;background:var(--accent)}.chip[data-region][data-before]::before {inset:-3px 0 auto;width:auto;height:2px}
      :host(:dir(rtl)) .chip:not([data-region])[data-before]::before {inset:-3px -3px -3px auto}.drop-end {height:26px;width:2px;background:var(--accent);align-self:center}.zone[data-edge=left] .drop-end,.zone[data-edge=right] .drop-end {grid-column:1/-1;width:100%;height:2px}
      .page {grid-column:2;grid-row:2;position:relative;display:flex;flex-direction:column;justify-content:center;align-items:center;padding:32px 18px;gap:22px;min-width:0;background:var(--preview-surface);color:var(--preview-page-ink)}
      .preview-customize {position:absolute;inset-inline-end:12px;bottom:12px;pointer-events:none;width:14px;height:14px}
      .preview-homebrand {display:flex;align-items:center;justify-content:center;gap:7px;font-size:22px;font-weight:500;letter-spacing:-.6px;max-width:100%;min-width:0}.preview-homebrand>span:last-child {min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.page-mark {width:28px;height:28px;flex:none;background:var(--preview-page-ink);mask:url(chrome://theme/IDR_PRODUCT_LOGO@2x) center/contain no-repeat}
      .preview-search {--preview-search-surface:color-mix(in srgb,var(--preview-page-ink) 6%,var(--preview-surface));display:flex;align-items:center;gap:9px;width:min(100%,46.667cqw);max-width:560px;min-width:0;min-height:36px;padding:9px 12px;border:1px solid color-mix(in srgb,var(--preview-page-ink) 32%,transparent);border-radius:6px;background:var(--preview-search-surface);color:var(--preview-page-ink);font-size:11px}.preview-search span {white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.preview-search .control-icon {width:14px;height:14px}
      :host([glass-enabled]) .preview-search {--preview-search-surface:transparent}
      .preview-favorites {display:flex;flex-direction:column;align-items:center;gap:10px;color:var(--preview-page-ink);max-width:100%;text-align:center;overflow-wrap:anywhere}.preview-favorites-title {font-size:10px;font-weight:400}.preview-shortcut {display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;width:72px;max-width:100%;min-height:64px;border:0;border-radius:6px;font-size:10px}.preview-shortcut span:first-child {display:grid;place-items:center;width:26px;height:26px;border-radius:6px;font-size:15px;background:color-mix(in srgb,var(--preview-page-ink) 6%,var(--preview-surface));color:var(--preview-page-ink)}
      :host([glass-enabled]) .preview-shortcut span:first-child {background:color-mix(in srgb,var(--preview-page-ink) 6%,transparent)}
      .tray {margin:18px 0;padding:16px;border:1px dashed var(--line);border-radius:10px;min-height:80px;transition:border-color 140ms ease,background-color 140ms ease}.tray[data-over] {border-color:var(--accent);background:color-mix(in srgb,var(--accent) 7%,transparent)}
      .selection-tools {scroll-margin-block:116px 160px;display:flex;flex-direction:column;gap:10px;padding:14px 0;width:100%;min-width:0}.selection-info {display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}.selection-title {display:flex;align-items:center;gap:7px;max-width:100%}.selection-name {font-size:13px;font-weight:500;overflow-wrap:anywhere}.selection-tools .muted,.restriction {font-size:11px;overflow-wrap:anywhere}.selection-tools button {font-size:11px;padding:6px 9px;min-height:30px}.selection-position {font-size:11px;color:var(--cr-secondary-text-color)}.selection-actions {display:flex;flex-wrap:wrap;gap:8px;align-items:center}.placement-options,.reorder-options {display:flex;flex-wrap:wrap;gap:4px}.placement-options [aria-pressed=true] {border-color:var(--accent);color:var(--accent)}
      .edge-target {white-space:nowrap;position:absolute;z-index:1;inset:4px auto auto 4px;font-size:10px;min-height:24px;padding:3px 5px;border-color:var(--accent);background:var(--preview-chrome);color:var(--preview-ink);opacity:0;pointer-events:none}.canvas[data-dragging] .edge-target,.edge-target:focus-visible {opacity:1;pointer-events:auto}.zone[data-edge=right] .edge-target {inset:4px 4px auto auto}.edge-target[aria-pressed=true] {color:var(--accent)}.edge-target:hover:enabled {border-color:var(--accent)}
      .empty-zone {position:absolute;inset:0;display:none;align-items:center;justify-content:center;font-size:10px;color:var(--accent);pointer-events:none;white-space:nowrap}.canvas[data-dragging] .zone:not([data-populated]) .empty-zone {display:flex}.canvas[data-dragging] .zone:not([data-populated]) {background:color-mix(in srgb,var(--accent) 7%,var(--preview-chrome))}.zone[data-edge=left] .empty-zone,.zone[data-edge=right] .empty-zone {writing-mode:vertical-rl}
      .drag-ghost {position:fixed;z-index:1000;pointer-events:none;display:flex;align-items:center;gap:8px;border:1px solid var(--accent);border-radius:8px;background:var(--cr-card-background-color);box-shadow:0 4px 16px #0003;padding:8px 12px;font-size:12px;max-width:200px}.chip[data-dragging] {opacity:.4}
      .tray-heading {display:flex;gap:12px;justify-content:space-between;align-items:center;flex-wrap:wrap;margin-bottom:14px}.tray-heading h3 {margin-bottom:3px}.count {margin-inline-start:4px;color:var(--cr-secondary-text-color);font-size:11px}.control-search {font:inherit;font-size:12px;color:inherit;background:var(--cr-card-background-color);border:1px solid var(--line);border-radius:8px;padding:8px 10px;max-width:100%;min-width:0;width:170px}.tray-hint {margin-top:12px}.keyboard-help {font-size:12px;color:var(--cr-secondary-text-color)} #individualControls > summary {cursor:pointer;padding:12px 0;font-weight:500} .keyboard-help summary {cursor:pointer;padding:8px 0;width:fit-content}.keyboard-help p {margin:6px 0}
      .footer {position:sticky;bottom:0;z-index:2;padding:14px 0 8px;margin-top:12px;border-top:1px solid var(--line);background:var(--cr-card-background-color)}.footer-actions {display:flex;gap:8px;flex-wrap:wrap;align-items:center}.footer button {font-size:12px}.spacer {flex:1}.status {font-size:12px;min-height:20px;margin-top:10px;color:var(--cr-secondary-text-color)}.notice {padding:12px;background:color-mix(in srgb,var(--accent) 8%,transparent);border-radius:8px;margin:12px 0}
      dialog {border:1px solid var(--line);border-radius:12px;background:var(--cr-card-background-color);color:inherit;max-width:380px;padding:24px}dialog::backdrop {background:#0007}.dialog-actions {display:flex;justify-content:end;gap:8px;margin-top:24px}
      @media(max-width:700px) {:host {padding:16px}.presets {grid-template-columns:repeat(2,minmax(0,1fr))}.guide {gap:8px}.spacer {display:none}.footer-actions {gap:6px}.footer button {font-size:11px;padding:7px}.control-search {width:100%}.tray-heading {width:100%}.canvas .control-icon {width:13px;height:13px}.canvas .chip[data-control=address] {min-width:0}.zone[data-populated] {padding:3px}.zone[data-edge=left] .chips,.zone[data-edge=right] .chips {grid-template-columns:repeat(auto-fill,minmax(22px,1fr))}.preview-tab {gap:4px;padding:5px 3px}.tab-close,.address-star {display:none}.page {padding:18px 6px;gap:18px}.preview-homebrand {font-size:16px;gap:4px}.page-mark {width:18px;height:18px}.preview-search {width:96%;padding:9px 6px;gap:4px;font-size:9px}.selection-actions {gap:6px}}
      .editor-heading {align-items:center;margin-bottom:16px}.editor-heading h2 {font-size:20px;font-weight:500;letter-spacing:-.25px}.editor-heading .muted {max-width:440px}
      .presets {grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-bottom:8px}.presets button {flex-direction:row;align-items:center;gap:10px;padding:8px;min-height:48px;font-size:12px;border-radius:8px}.preset-preview {width:36px;height:28px;flex:none;margin:0}.preset-preview span:first-child {width:7px}.preset-preview span:last-child {inset-inline-start:13px}.preset-name {line-height:1.4}.preset-description {position:absolute;clip-path:inset(50%);width:1px;height:1px;overflow:hidden}.preset-summary {margin-bottom:12px;min-height:18px}
      .position-controls {gap:10px 16px;margin-bottom:12px}.position-control {display:flex;align-items:center;gap:8px;padding:0;border:0}.position-label {width:76px;flex:none;flex-wrap:wrap;row-gap:2px;margin:0;line-height:1.4}.position-options {flex:1;min-width:0}.position-options button {padding:6px 5px}.position-hint {position:absolute;width:1px;height:1px;clip-path:inset(50%);overflow:hidden}
      .preview-library {gap:4px !important}.preview-library-tile {height:20px}.preview-library-row {height:18px}.zone:is([data-edge=left],[data-edge=right]):has(.chip:is([data-control=favorites],[data-control=bookmarks])) .chip[data-control=tabs] {min-height:78px}
      .selection-tools {padding:12px 14px;border:0;border-top:1px solid var(--line);border-radius:0;margin:0;gap:8px;scroll-margin-block:90px 140px;background:var(--cr-card-background-color)}.selection-eyebrow {font-size:10px;color:var(--cr-secondary-text-color);margin:0}.selection-info {display:block}.selection-title {margin-bottom:4px}.selection-actions {margin:2px 0 0;gap:8px}.selection-info .muted {font-size:11px}.selection-position {border-inline-start:1px solid var(--line);padding-inline-start:8px}
      .library-heading {margin:20px 0 10px}.library-options {gap:10px;margin-bottom:20px}.library-option {padding:10px}.library-option p {line-height:1.4}.tray {border-style:solid;border-radius:10px;padding:14px}.tray-heading {gap:12px}.tray-heading .muted {font-size:11px}.control-groups {display:grid;gap:16px;margin-top:16px}.control-group h4 {margin:0 0 8px;font-size:11px;font-weight:500;color:var(--cr-secondary-text-color)}.control-group .chip {background:var(--cr-hover-background-color);border-color:transparent;min-height:34px}.control-group .chip:not([aria-pressed=true]):hover:enabled {border-color:var(--line)}.control-group .chip[aria-pressed=true] {border-color:var(--accent);background:color-mix(in srgb,var(--accent) 10%,var(--cr-card-background-color))}.tray-hint {font-size:11px;margin-top:14px}.control-search {border-radius:8px;min-height:36px}
      .footer {padding-top:12px}.footer-actions {gap:6px}.footer-actions button {font-size:12px}.status {font-size:11px;line-height:1.5}
      @media(max-width:560px) {.presets {grid-template-columns:repeat(2,minmax(0,1fr))}.presets button {min-height:48px}.position-controls {grid-template-columns:1fr}.position-label {width:84px}.tray-heading {align-items:stretch}.control-search {width:100%;max-width:none}.library-options {grid-template-columns:1fr}.selection-actions {gap:8px}}
      @container(max-width:560px) {
        .zone[data-edge=top][data-populated] {padding-inline:4px}
        .canvas .zone[data-edge=top] .chip:not([data-control=tabs]):not([data-control=address]) {width:22px;flex-basis:22px}
        .canvas .chip {min-height:22px;height:22px;padding:3px}
        .canvas .control-icon {width:13px;height:13px}
        .address-slot {min-width:70px;flex-basis:70px}.address-star {display:none}
        .address-text {font-size:9px}.page {padding:28px 10px;gap:18px}
        .preview-homebrand {font-size:17px;gap:5px}.page-mark {width:22px;height:22px}
        .preview-search {width:100%;min-height:32px;padding:7px;gap:5px;font-size:10px}
        .preview-favorites-title,.preview-shortcut {font-size:9px}
      }
      /* Match the native sidebar hierarchy without changing draft placement.
         Pinned sites are examples within the tab region. */
      .canvas .zone:is([data-edge=left],[data-edge=right])[data-populated] {padding:0;border-width:0}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips:not([data-tabs-group]) {display:flex;flex-wrap:wrap;gap:0;justify-content:start;padding-inline:0}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips:not([data-tabs-group]) .chip[data-region] {flex:0 0 100%}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .chip:not([data-region]) {flex:0 0 min(36px,25%);width:min(36px,25%);height:36px;min-height:36px;padding:9px 5px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .control-icon {width:16px;height:16px}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips:not([data-tabs-group]) .chip:is([data-control=extensions],[data-control=additional-actions]) {flex:0 0 min(36px,25%);width:min(36px,25%);height:36px;min-height:36px;justify-content:center;padding:9px 5px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) {container:sidebar-preview / inline-size}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips:not([data-tabs-group]) .chip:is([data-control=favorites],[data-control=bookmarks]) {flex-basis:calc(100% + 12px);max-width:none;width:calc(100% + 12px);margin-inline:-6px}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips[data-sidebar-address] {padding-bottom:4px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=address] {height:38px;min-height:38px;margin-block:0;padding:8px;border-color:transparent;border-radius:14px;background:color-mix(in srgb,var(--preview-ink) 10%,transparent)}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=address] .address-text {text-align:start}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=address][aria-pressed=true] {outline:1px solid var(--accent);outline-offset:-1px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=tabs] {padding:0 5px;gap:0}
      .canvas .zone:is([data-edge=left],[data-edge=right])>.chips .chip[data-control=new-tab] {flex:0 0 100%;width:100%;height:32px;min-height:32px;justify-content:start;padding:7px 13px;color:color-mix(in srgb,var(--preview-ink) 70%,transparent)}
      .canvas .control-label[data-new-tab-label] {position:static;width:auto;height:auto;clip-path:none}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .preview-tab {height:36px;padding:6px 8px;border-block:2px solid transparent;background-clip:padding-box;border-radius:10px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .preview-tab .control-icon {width:16px;height:16px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .preview-tab[data-active] {background:color-mix(in srgb,var(--preview-ink) 12%,var(--preview-chrome));background-clip:padding-box;box-shadow:0 .8px 1.5px #0001}
      .preview-pinned {display:flex;align-items:center;gap:4px;grid-column:1/-1;width:100%;padding:6px 0 2px}
      .preview-pinned>span {display:grid;place-items:center;flex:1;min-width:0;height:40px;border-radius:10px;background:color-mix(in srgb,var(--preview-ink) 10%,transparent)}
      .preview-pinned .tab-mark {width:16px;height:16px}
      .preview-library-row[data-nested] {padding-inline-start:15px}
      .preview-command-popup {position:absolute;inset-inline:8px;top:50%;transform:translateY(-50%);margin-inline:auto;max-width:640px;min-height:48px;display:flex;align-items:center;gap:10px;padding:12px 14px;border:1px solid var(--cr-separator-color);border-radius:12px;background:var(--preview-chrome);color:var(--preview-ink);box-shadow:0 6px 20px #0003;font-size:11px;pointer-events:none;z-index:1}
      .preview-command-popup>span {flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
      .preview-command-popup .control-icon {width:14px;height:14px}
      .space-footer {display:flex;align-items:center;justify-content:center;gap:0;padding:0}
      .space-footer .chip[data-control=spaces] {container:spaces-preview / inline-size;flex:1;min-width:0;width:auto;height:32px;min-height:32px;padding:4px;border-width:0;gap:0}
      .space-footer .chip[data-control=spaces][aria-pressed=true] {outline:1px solid var(--accent);outline-offset:-1px}
      .space-footer .preview-space-dots {display:grid;grid-auto-flow:column;grid-auto-columns:24px;align-items:center;justify-items:center;gap:0}
      .space-footer .preview-space-more {display:grid;place-items:center;flex:none;width:24px;height:24px;font-size:14px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .space-footer .chip:is([data-control="action:bookmarks"],[data-control=menu]) {flex:none;width:36px;height:36px;min-height:36px;margin:0;padding:9px}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .space-footer .chip[data-control="action:bookmarks"] {margin-inline-end:auto}
      .canvas .zone:is([data-edge=left],[data-edge=right]) .space-footer .chip[data-control=menu] {margin-inline-start:auto}
      .space-footer .chip[data-control=menu] .control-icon {transform:rotate(90deg)}
      @container spaces-preview (max-width:71px) {
        .space-footer[data-menu] .preview-space-dot:nth-child(n+3) {display:none}
      }
      @container spaces-preview (max-width:95px) {
        .space-footer:not([data-menu]) .preview-space-dot:nth-child(n+3) {display:none}
      }
      @container spaces-preview (max-width:47px) {
        .space-footer[data-menu] .preview-space-dot:nth-child(n+2) {display:none}
      }
      @container spaces-preview (max-width:71px) {
        .space-footer:not([data-menu]) .preview-space-dot:nth-child(n+2) {display:none}
      }
      @container sidebar-preview (max-width:86px) {
        .space-footer {flex-direction:column;padding-inline:4px}
        .space-footer .chip[data-control=spaces] {flex:none;width:100%}
        .canvas .zone:is([data-edge=left],[data-edge=right]) .space-footer .chip:is([data-control="action:bookmarks"],[data-control=menu]) {margin-inline:0}
        .space-footer:not([data-menu]) .chip[data-control=spaces] {flex-direction:column;height:auto;padding:0}
        .space-footer:not([data-menu]) .preview-space-dots {grid-auto-flow:row;grid-auto-columns:auto;grid-auto-rows:36px}
        .space-footer:not([data-menu]) .preview-space-dot:nth-child(n) {display:block}
        .space-footer:not([data-menu]) .preview-space-more {height:36px}
        .canvas .zone:is([data-edge=left],[data-edge=right]) .chip:is([data-control=address],[data-control=new-tab]) {justify-content:center}
        .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=address] .address-text,
        .canvas .zone:is([data-edge=left],[data-edge=right]) .chip[data-control=address] .address-star,
        .canvas .zone:is([data-edge=left],[data-edge=right]) .preview-tab-label,
        .canvas .zone:is([data-edge=left],[data-edge=right]) .tab-close {display:none}
        .canvas .zone:is([data-edge=left],[data-edge=right]) .control-label[data-new-tab-label] {position:absolute;width:1px;height:1px;overflow:hidden;clip-path:inset(50%)}
        .canvas .zone:is([data-edge=left],[data-edge=right]) .preview-tab {justify-content:center}
        .preview-pinned {flex-direction:column}
        .preview-pinned>span {width:100%;flex:0 0 40px}
        .canvas .zone:is([data-edge=left],[data-edge=right])>.chips:not([data-tabs-group]) .chip:is([data-control=back],[data-control=forward],[data-control=reload],[data-control=extensions]) {display:none}
        .space-footer[data-menu] .preview-space-dot:not([data-active]) {display:none}
      }
      @media(forced-colors:active) {
        .canvas {--preview-divider:CanvasText;--preview-chrome:Canvas;--preview-surface:Canvas;--preview-ink:CanvasText;--preview-page-ink:CanvasText}
        .canvas .chip[aria-pressed=true] {outline:2px solid Highlight;outline-offset:-2px}
        .canvas .chip[data-control=address],.preview-search {border-color:CanvasText}
      }
      .canvas[data-comfortable] .zone:is([data-edge=left],[data-edge=right]) .preview-tab {height:44px}
      .canvas[data-comfortable] .preview-pinned>span {height:48px}
      @container sidebar-preview (max-width:86px) {
        .canvas[data-comfortable] .preview-pinned>span {flex-basis:48px}
      }
      @media(prefers-reduced-motion:reduce) {* {scroll-behavior:auto;transition:none !important;animation:none !important}}
      @media(max-width:700px) {.library-options {grid-template-columns:minmax(0,1fr);gap:8px}.position-controls {grid-template-columns:minmax(0,1fr);gap:8px}.position-control {padding:10px}.position-label {margin-bottom:7px}}
    `;
  }
  override render() { return getHtml.call(this); }
  presetNames(): string[] { return ['minimal-left', 'minimal-right', 'sidebar-top', 'classic-top', 'compact-top'].filter(name => !!this.presets[name]); }
  edges(): DockEdge[] { return EDGES; }
  previewSideSize(edge: DockEdge): string {
    const items = this.items(edge);
    if (!items.length) return this.dragGhost ? '36px' : '0px';
    return items.some(item => ['tabs', 'address', 'spaces', 'favorites', 'bookmarks'].includes(item.id)) ?
        'clamp(48px, 24%, 144px)' : '36px';
  }
  hasSidebarAddress(edge: DockEdge): boolean {
    if (edge !== 'left' && edge !== 'right') return false;
    return this.items(edge).some(item => item.id === 'address');
  }
  commandPopupVisible(): boolean {
    return this.selected === 'address' && this.edges().some(edge => this.hasSidebarAddress(edge));
  }
  footerItems(edge: DockEdge): LayoutItem[] {
    return edge === 'top' ? [] : ['action:bookmarks', 'spaces', 'menu'].flatMap(id =>
      this.items(edge).filter(item => item.id === id));
  }
  previewGroups(edge: DockEdge): Array<{items: LayoutItem[]; tabs: boolean; last: boolean; sidebarAddress?: boolean; pinned?: boolean}> {
    const side = edge === 'left' || edge === 'right';
    const footer = new Set(this.footerItems(edge).map(item => item.id));
    const items = this.items(edge).filter(item => !footer.has(item.id));
    if (!side) {
      const firstNavigation = items.findIndex(item => ['back', 'forward', 'reload', 'address'].includes(item.id));
      if (items[0]?.id === 'tabs' && firstNavigation > 0) {
        return [{items: items.slice(0, firstNavigation), tabs: true, last: false},
          {items: items.slice(firstNavigation), tabs: false, last: true}];
      }
      return [{items, tabs: false, last: true}];
    }
    const tabs = items.findIndex(item => item.id === 'tabs');
    const address = this.draft?.current.items.find(
        item => item.id === 'address' && item.edge === edge);
    // Hidden controls have no visual order; the draft reducer moves them last.
    // Match native grouping while retaining visible Address placement choices.
    const newTab = tabs >= 0 && address &&
            (!address.visible || address.order < items[tabs].order) &&
            items[tabs + 1]?.id === 'new-tab' ?
        items[tabs + 1] :
        undefined;
    const groups: ReturnType<NoventraLayoutEditorElement['previewGroups']> = [];
    let controls: LayoutItem[] = [];
    const flushControls = () => {
      if (controls.length) groups.push({items: controls, tabs: false, last: false});
      controls = [];
    };
    for (const item of items) {
      if (item === newTab) continue;
      if (item.id === 'address' || item.id === 'tabs') {
        flushControls();
        if (item.id === 'tabs') {
          groups.push({items: [], tabs: false, last: false, pinned: true});
        }
        groups.push({items: [item], tabs: item.id === 'tabs', last: false,
          sidebarAddress: item.id === 'address'});
        if (item.id === 'tabs' && newTab) {
          groups.push({items: [newTab], tabs: false, last: false});
        }
      } else {
        controls.push(item);
      }
    }
    flushControls();
    if (groups.length) groups[groups.length - 1].last = true;
    return groups;
  }
  async revealSpaceSwitcher(): Promise<void> {
    const spaces = this.draft?.current.items.find(item => item.id === 'spaces');
    if (!this.draft || !spaces || !this.capabilities.some(cap => cap.id === 'spaces')) {
      throw new Error('The workspace switcher is not available yet. Wait for the layout to load or reload saved changes.');
    }
    this.selected = 'spaces';
    const tabs = this.draft.current.items.find(item => item.id === 'tabs' && item.visible);
    const edge = tabs && (tabs.edge === 'left' || tabs.edge === 'right') ? tabs.edge :
        spaces.edge === 'right' ? 'right' : 'left';
    if (this.conflict) {
      this.message = 'The saved layout changed. Reload the current layout before showing Spaces.';
    } else if (!this.canPlaceControl('spaces', edge)) {
      this.message = 'Your organization controls the workspace switcher. Its placement is unchanged.';
    } else {
      this.act({kind: 'move', id: 'spaces', edge});
      const revealed = this.draft.current.items.find(item => item.id === 'spaces');
      if (revealed?.visible && revealed.edge === edge) {
        await this.whenSaved();
        if (this.dirty() || this.conflict || this.saveFailed) {
          throw new Error('The workspace switcher could not be saved. Resolve the layout error and try again.');
        }
        this.message = 'The workspace switcher is visible in the sidebar.';
      }
    }
    await this.updateComplete;
    this.focusSelected();
  }
  selection(): Array<{item: LayoutItem; cap: LayoutCapability}> {
    const item = this.selectedItem(); const cap = this.selectedCapability();
    return item && cap ? [{item, cap}] : [];
  }
  isRegion(id: string): boolean { return ['address', 'tabs', 'spaces', 'favorites', 'bookmarks', 'extensions', 'additional-actions'].includes(id); }
  iconPath(id: string): string {
    return ({
      back: 'M19 12H5m6-6-6 6 6 6', forward: 'M5 12h14m-6-6 6 6-6 6',
      reload: 'M19 8a8 8 0 1 0 1 8M19 3v5h-5', home: 'm3 11 9-8 9 8M6 9v12h5v-7h3v7h4V9',
      address: 'M15 10a5 5 0 1 1-10 0 5 5 0 0 1 10 0m-1 4 6 6',
      tabs: 'M4 4h16v16H4zM9 4v16', spaces: 'M4 6h6v6H4zM14 6h6v6h-6zM4 16h16',
      favorites: 'm12 3 3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1z',
      bookmarks: 'M6 3h12v18l-6-4-6 4z',
      'saved-sites': 'm12 3 3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1z',
      'new-tab': 'M12 4v16M4 12h16', menu: 'M12 5h.01M12 12h.01M12 19h.01',
      'action:settings': 'M10 3h4l1 3 3 1 3 3v4l-3 1-1 3-3 3h-4l-1-3-3-1-3-3v-4l3-1 1-3zM16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0',
      customize: 'M4 6h16M4 12h16M4 18h16M8 3v6M16 9v6M10 15v6',
      extensions: 'M4 8h5V5a3 3 0 0 1 6 0v3h5v5h-3a3 3 0 0 0 0 6h3v3H4z',
      profile: 'M16 7a4 4 0 1 1-8 0 4 4 0 0 1 8 0M4 21v-2a8 6 0 0 1 16 0v2',
      media: 'm8 4 12 8-12 8z', 'action:history': 'M4 5v5h5M4 10a8 8 0 1 1 0 6M12 7v5l4 2',
      'action:downloads': 'M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5',
      'action:bookmarks': 'M6 3h12v18l-6-4-6 4z', 'action:print': 'M6 8V3h12v5M6 17H3V8h18v9h-3M6 14h12v7H6z',
      'action:copy-url': 'm9 15 6-6M8 17l-1 1a4 4 0 0 1-6-6l5-5a4 4 0 0 1 6 0m4 0 1-1a4 4 0 0 1 6 6l-5 5a4 4 0 0 1-6 0',
      'action:save-page': 'M5 3h10l4 4v14H5zM9 3v6h6V3M9 21v-7h6v7',
      'action:find': 'M14 10a5 5 0 1 1-10 0 5 5 0 0 1 10 0m-1 4 7 7M3 3h16',
      'action:zoom-in': 'M14 10a5 5 0 1 1-10 0 5 5 0 0 1 10 0m-1 4 7 7M7 10h4M9 8v4',
      'action:zoom-out': 'M14 10a5 5 0 1 1-10 0 5 5 0 0 1 10 0m-1 4 7 7M7 10h4',
      'action:zoom-reset': 'M14 10a5 5 0 1 1-10 0 5 5 0 0 1 10 0m-1 4 7 7M7 8h4v4H7z',
      'action:fullscreen': 'M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5',
      'action:reading-mode': 'M3 4h6l3 2 3-2h6v15h-6l-3 2-3-2H3zM12 6v15',
      'action:task-manager': 'M3 4h18v16H3zM7 16v-4M12 16V8M17 16v-6',
      'action:devtools': 'm8 6-6 6 6 6M16 6l6 6-6 6M14 4l-4 16',
      'action:passwords': 'M10 8a4 4 0 1 1-8 0 4 4 0 0 1 8 0M9 11l11 9M14 16l2-2M17 19l2-2',
      'action:bookmark-manager': 'M5 3h14v18l-7-4-7 4zM8 7h8M8 11h8',
    } as Record<string, string>)[id] || 'M4 12h2m5 0h2m5 0h2';
  }
  private controlId(event: Event): string { return (event.currentTarget as HTMLElement).dataset['control'] || ''; }
  onSelectClick(event: Event) {
    this.selected = this.controlId(event);
    if ((event.currentTarget as HTMLElement).closest('.tray')) {
      void this.updateComplete.then(() => this.shadowRoot.querySelector('.selection-tools')?.scrollIntoView({
        block: 'nearest', behavior: this.motionMedia_.matches ? 'instant' : 'smooth',
      }));
    }
  }
  onChipKeydown(event: KeyboardEvent) { this.chipKey(event, this.controlId(event)); }
  onControlPointerdown(event: PointerEvent) {
    if (event.button !== 0 || this.controlsDisabled()) return;
    this.selected = this.controlId(event);
    (event.currentTarget as HTMLElement).focus({preventScroll: true});
    this.pointer_ = {id: event.pointerId, control: this.selected, x: event.clientX, y: event.clientY,
      currentX: event.clientX, currentY: event.clientY, moving: false};
    this.setPointerCapture(event.pointerId);
  }
  onPresetClick(event: Event) { this.preset((event.currentTarget as HTMLElement).dataset['preset'] || ''); }
  onEdgeClick(event: Event) {
    this.move((event.currentTarget as HTMLElement).dataset['placeEdge'] as DockEdge);
  }
  canPlace(edge: DockEdge): boolean {
    return this.canPlaceControl(this.selected, edge);
  }
  canPlaceControl(id: string, edge: DockEdge): boolean {
    const cap = this.capabilities.find(c => c.id === id);
    return !!cap && !this.controlsDisabled() &&
        cap.requiredVisibility !== false && EDGES.includes(edge) && cap.edges.includes(edge);
  }
  positionItem(id: string): LayoutItem|undefined {
    return this.draft?.current.items.find(item => item.id === id);
  }
  navigationChoices(): NavigationPlacement[] { return ['top', 'sidebar']; }
  canPlaceNavigation(placement: NavigationPlacement): boolean {
    return !!this.draft && !this.controlsDisabled() && canPlaceNavigation(this.draft.current, this.capabilities, placement);
  }
  navigationMatches(placement: NavigationPlacement): boolean {
    return !!this.draft && navigationMatches(this.draft.current, placement);
  }
  navigationHint(): string {
    const side = this.draft && navigationEdge(this.draft.current, 'sidebar');
    return (side ? '' : 'Place tabs on a side to move navigation with them. ') +
        'Move Back, Forward and Reload together. The sidebar keeps them above your tabs, with New Tab below.';
  }
  onNavigationClick(event: Event) {
    const placement = (event.currentTarget as HTMLElement).dataset['navigationPlacement'] as NavigationPlacement;
    if (this.canPlaceNavigation(placement)) {
      this.act({kind: 'navigation', placement});
    }
  }
  onPositionClick(event: Event) {
    const button = event.currentTarget as HTMLElement;
    const id = button.dataset['positionControl'] || '';
    const edge = button.dataset['positionEdge'] as DockEdge;
    const item = this.positionItem(id);
    if (!item || !this.canPlaceControl(id, edge) ||
        (item.visible && item.edge === edge)) return;
    this.selected = id;
    this.move(edge);
  }
  edgeLabel(edge: DockEdge): string {
    return {top: 'Top toolbar', left: 'Left sidebar', right: 'Right sidebar'}[edge];
  }
  onQuickEdgeClick(event: Event) {
    this.move((event.currentTarget as HTMLElement).dataset['quickEdge'] as DockEdge);
  }
  canReorder(offset: number): boolean {
    const item = this.selectedItem();
    if (!item?.visible || this.controlsDisabled()) return false;
    const peers = this.reorderPeers(item);
    const index = peers.findIndex(i => i.id === item.id) + offset;
    return index >= 0 && index < peers.length;
  }
  private reorderPeers(item: LayoutItem): LayoutItem[] {
    if (item.id === 'spaces') return [];
    return this.items(item.edge).filter(peer => peer.id !== 'spaces');
  }
  onMoveEarlierClick() { this.reorder(-1); }
  onMoveLaterClick() { this.reorder(1); }
  presetLabel(name: string): string {
    return ({workspace: 'Workspace', 'sidebar-top': 'Sidebar & top bar', 'minimal-left': 'Left sidebar', 'minimal-right': 'Right sidebar',
      'classic-top': 'Normal browser', 'compact-top': 'Compact top'} as Record<string, string>)[name] || name;
  }
  presetDescription(name: string): string {
    return ({workspace: 'Tabs and quick bookmarks; address entry on demand', 'sidebar-top': 'Bookmarks & tabs beside your page', 'minimal-left': 'Pinned sites and tabs, with New Tab below', 'minimal-right': 'The same sidebar controls on the right',
      'classic-top': 'Tabs above a separate address and navigation toolbar',
      'compact-top': 'Navigation, address and tabs in one compact row'} as Record<string, string>)[name] || '';
  }
  presetMatches(name: string): boolean {
    const preset = this.presets[name];
    const visible = (layout: BrowserLayout) => JSON.stringify(this.edges().map(edge =>
      visibleItems(layout, edge).map(i => i.id)));
    return !!preset && !!this.draft && visible(preset) === visible(this.draft.current);
  }
  controlDescription(id: string): string {
    return ({
             address:
                 'Search the web or enter a website address. Sidebar presets hide this row; press Ctrl+L for centered editing or add the address row to your layout.',
             tabs:
                 'Your open pages and pinned site cards. A combined sidebar uses compact rows with New Tab below them. Workspace dots remain at the bottom; other arrangements keep their control order. Normal browser puts top tabs above the toolbar; Compact top shares one row.',
             bookmarks:
                 'An optional section for saved pages and expandable folders. The sidebar presets use the searchable Bookmarks popup instead.',
             spaces:
                 'Switch between browsing spaces and separate logins. Always at the bottom of the selected sidebar.',
             extensions:
                 'Open the Extensions menu and use pinned extensions. On an icon-only sidebar, find them in the bottom browser menu.',
             'additional-actions': 'Additional pinned tools move together.',
             'new-tab': 'Open a fresh tab in your current space.',
             menu: 'Open browser actions and settings.',
             customize: 'Return here to adjust your layout.',
             profile: 'Manage your browser profile.',
             'saved-sites': 'Open the bookmark manager from a star shortcut.',
             'action:bookmarks':
                 'Open saved pages and folders in a compact searchable popup or persistent side panel, using your Sidebar preference. The footer button stays available when no bookmarks are saved.',
             'action:bookmark-manager':
                 'Open the bookmark manager from a labeled tool.',
             back: 'Return to the previous page.',
             forward: 'Go forward in this tab.',
             reload: 'Refresh the current page.',
             home: 'Open your chosen home page.'
           } as Record<string, string>)[id] ||
        'Place this tool where it is easiest to reach.';
  }
  onFilterInput(event: Event) { this.filter = (event.target as HTMLInputElement).value; }
  availableItems(): LayoutItem[] {
    const query = this.filter.trim().toLocaleLowerCase();
    const words = query.split(/\s+/).filter(Boolean);
    return this.items('tray').filter(item => {
      // Keep saved aliases valid; offer one new full-page manager control.
      if (item.id === 'action:bookmark-manager') return false;
      const text = `${this.label(item.id)} ${this.controlDescription(item.id)} ${this.controlGroup(item.id)}`.toLocaleLowerCase();
      return words.every(word => text.includes(word));
    });
  }
  currentPresetDescription(): string {
    const name = this.presetNames().find(preset => this.presetMatches(preset));
    return name ? this.presetDescription(name) : 'Custom layout. Click any control in the preview to adjust it.';
  }
  private controlGroup(id: string): string {
    if (['back', 'forward', 'reload', 'home', 'address'].includes(id)) return 'Navigation';
    if (['tabs', 'new-tab', 'spaces'].includes(id)) return 'Tabs & spaces';
    if (['favorites', 'bookmarks', 'saved-sites', 'action:bookmarks', 'action:bookmark-manager'].includes(id)) return 'Saved pages';
    if (['menu', 'customize', 'profile', 'extensions', 'additional-actions', 'action:settings'].includes(id)) return 'Browser & extensions';
    return 'Page tools';
  }
  availableGroups(): Array<{label: string; items: LayoutItem[]}> {
    const items = this.availableItems();
    return ['Navigation', 'Tabs & spaces', 'Saved pages', 'Browser & extensions', 'Page tools']
        .map(label => ({label, items: items.filter(item => this.controlGroup(item.id) === label)}))
        .filter(group => group.items.length > 0);
  }
  onHideClick() { this.act({kind: 'hide', id: this.selected}); }
  onUndoClick() { this.act({kind: 'undo'}); }
  onRedoClick() { this.act({kind: 'redo'}); }
  onResetClick() { this.preset('workspace'); }
  onDialogCancel(event: Event) { event.preventDefault(); this.onKeepEditingClick(); }
  static override get properties() {
    return {draft: {type: Object}, capabilities: {type: Array}, presets: {type: Object},
      selected: {type: String}, message: {type: String}, conflict: {type: Boolean}, saveFailed: {type: Boolean},
      applying: {type: Boolean}, reloading: {type: Boolean}, pendingNavigation: {type: Object}, dropTarget: {type: Object},
      dragGhost: {type: Object}, filter: {type: String},
      glassEnabled: {type: Boolean, attribute: 'glass-enabled', reflect: true},
      tabDensity: {type: Number}, bookmarkOpening: {type: Number},
      searchMode: {type: Boolean}};
  }
  accessor draft: LayoutDraft|null = null;
  accessor capabilities: LayoutCapability[] = [];
  accessor presets: Record<string, BrowserLayout> = {};
  accessor selected = 'back';
  accessor filter = '';
  accessor glassEnabled = false;
  accessor tabDensity = 0;
  accessor bookmarkOpening = 0;
  accessor searchMode = false;
  accessor dragGhost: {x: number; y: number; id: string}|null = null;
  accessor message = '';
  accessor conflict = false;
  accessor applying = false;
  accessor reloading = false;
  accessor saveFailed = false;
  private savePromise_: Promise<void>|null = null;
  accessor pendingNavigation: (() => void)|null = null;
  accessor dropTarget: {edge: DockEdge|'tray'; before?: string}|null = null;
  private motionMedia_ = matchMedia('(prefers-reduced-motion: reduce)');
  private animations_ = new Set<Animation>();
  private readonly onMotionChange_ = () => {
    if (this.motionMedia_.matches) this.animations_.forEach(animation => animation.cancel());
  };
  private listener_: WebUiListener|null = null;
  private latest_: LayoutState|null = null;
  private pointer_: {id: number; control: string; x: number; y: number;
    currentX: number; currentY: number; moving: boolean}|null = null;
  private scrollFrame_: number|null = null;
  private scrollTime_ = 0;
  private deferredState_: LayoutState|null = null;
  private navigationFocus_: HTMLElement|null = null;
  override connectedCallback() {
    super.connectedCallback();
    this.motionMedia_.addEventListener('change', this.onMotionChange_);
    this.listener_ = addWebUiListener('noventra-layout-changed', (state: LayoutState) => this.receive(state));
    this.addEventListener('pointermove', this.onPointerMove_);
    this.addEventListener('pointerup', this.onPointerUp_);
    this.addEventListener('pointercancel', this.onPointerCancel_);
    this.addEventListener('lostpointercapture', this.onPointerCancel_);
    this.addEventListener('keydown', this.onEditorKeydown_);
    window.addEventListener('beforeunload', this.beforeUnload_);
    window.addEventListener('noventra-before-settings-navigation', this.beforeNavigation_);
    void sendWithPromise<LayoutState>('noventraGetLayout').then(state => {
      if (this.isConnected) this.receive(state);
    }).catch(() => { this.message = 'Could not load the layout. Reopen Customize Noventra to retry.'; });
  }
  override disconnectedCallback() {
    if (this.listener_) removeWebUiListener(this.listener_);
    this.listener_ = null;
    this.motionMedia_.removeEventListener('change', this.onMotionChange_);
    this.animations_.forEach(animation => animation.cancel());
    window.removeEventListener('beforeunload', this.beforeUnload_);
    window.removeEventListener('noventra-before-settings-navigation', this.beforeNavigation_);
    this.removeEventListener('pointermove', this.onPointerMove_);
    this.removeEventListener('pointerup', this.onPointerUp_);
    this.removeEventListener('pointercancel', this.onPointerCancel_);
    this.removeEventListener('lostpointercapture', this.onPointerCancel_);
    this.removeEventListener('keydown', this.onEditorKeydown_);
    this.cancelPointer_();
    super.disconnectedCallback();
  }
  dirty(): boolean { return !!this.draft && isDirty(this.draft); }
  managed(): boolean { return this.capabilities.some(c => c.managed); }
  selectedItem(): LayoutItem|undefined { return this.draft?.current.items.find(i => i.id === this.selected); }
  selectedCapability(): LayoutCapability|undefined { return this.capabilities.find(c => c.id === this.selected); }
  items(edge: DockEdge|'tray'): LayoutItem[] {
    if (!this.draft) return [];
    return edge === 'tray' ? this.draft.current.items.filter(i => !i.visible).sort((a, b) => a.order - b.order) :
        visibleItems(this.draft.current, edge);
  }
  label(id: string): string {
    return ({'saved-sites': 'Bookmark manager',
      'action:bookmark-manager': 'Bookmark manager',
      'action:bookmarks': 'Bookmarks button', bookmarks: 'Inline bookmarks'} as Record<string, string>)[id] ||
        this.capabilities.find(c => c.id === id)?.label || id;
  }
  receive(state: LayoutState) {
    if (this.applying) {
      if (!this.deferredState_ || state.layout.revision >= this.deferredState_.layout.revision) {
        this.deferredState_ = state;
      }
      return;
    }
    this.acceptState_(state);
  }
  private acceptState_(state: LayoutState) {
    // A read or event started before our last save can arrive afterwards.
    if (this.draft && state.layout.revision < this.draft.saved.revision) return;
    this.latest_ = state;
    this.capabilities = state.capabilities;
    this.presets = {...state.presets};
    if (state.presets['minimal-left']) {
      this.presets['workspace'] = structuredClone(state.presets['minimal-left']);
    } else if (state.presets['sidebar-top']) {
      this.presets['workspace'] = workspacePreset(state.presets['sidebar-top']);
    }
    if (!this.draft) {
      this.draft = createDraft(state.layout);
      this.conflict = false;
      return;
    }
    const catalogChanged = state.capabilities.some(c => !this.draft!.current.items.some(i => i.id === c.id)) ||
        this.draft.current.items.some(i => !state.capabilities.some(c => c.id === i.id));
    if (!this.dirty()) {
      // Our save acknowledgement must leave Undo and Redo available. A genuinely
      // newer external layout starts a new history rather than undoing it.
      if (state.layout.revision !== this.draft.saved.revision || catalogChanged) {
        this.draft = createDraft(state.layout);
      } else {
        this.draft = reconcileDraft(this.draft, state.layout, state.capabilities);
      }
      this.conflict = false;
      this.saveFailed = false;
    } else if (state.layout.revision !== this.draft.saved.revision || catalogChanged) {
      this.conflict = true;
      this.message = 'The saved layout changed while you were editing. Your latest choice has not been saved. Reload the current layout to continue.';
    } else {
      const next = reconcileDraft(this.draft, state.layout, state.capabilities);
      if (next !== this.draft) {
        this.draft = next;
        this.message = 'Organization settings changed. Required controls were updated.';
      }
      if (this.managed() && this.dirty()) {
        this.conflict = true;
        this.message = 'Your organization now controls the layout. Reload the current layout to continue.';
      }
    }
  }
  controlsDisabled(): boolean { return this.managed() || this.conflict || this.reloading; }
  savingStatus(): string {
    return this.reloading ? 'Loading…' : this.conflict ? 'Layout changed' : this.saveFailed ? 'Not saved' :
        this.applying || this.dirty() ? 'Saving…' : 'Layout saved';
  }
  act(action: DraftAction) {
    if (!this.draft || this.controlsDisabled()) return;
    try {
      const previous = this.draft;
      this.draft = reduceDraft(this.draft, action, this.capabilities);
      if (this.draft !== previous) {
        void this.updateComplete.then(() => this.animatePlacement_());
      }
      if (!this.dirty() && !this.applying) return;
      this.saveFailed = false;
      this.message = 'Saving layout…';
      void this.saveLayout_();
    } catch (error) { this.message = error instanceof Error ? error.message : String(error); }
  }
  private animatePlacement_() {
    if (!this.isConnected || this.motionMedia_.matches) return;
    const control = this.shadowRoot.querySelector<HTMLElement>(`[data-control="${CSS.escape(this.selected)}"]`);
    if (!control) return;
    this.animations_.forEach(animation => animation.cancel());
    const animation = control.animate([{opacity: .5, transform: 'translateY(5px)'},
      {opacity: 1, transform: 'translateY(0)'}], {duration: 180, easing: 'ease-out'});
    this.animations_.add(animation);
    const remove = () => { this.animations_.delete(animation); };
    void animation.finished.then(remove, remove);
  }
  move(edge: DockEdge) {
    const item = this.selectedItem();
    // Placement controls select an edge; reordering is an explicit action.
    if (item?.visible && item.edge === edge) return;
    this.act({kind: 'move', id: this.selected, edge});
  }
  reorder(offset: number) {
    const item = this.selectedItem();
    if (!item?.visible) return;
    const peers = this.reorderPeers(item);
    const index = peers.findIndex(i => i.id === item.id);
    if (index < 0 || index + offset < 0 || index + offset >= peers.length) return;
    this.act({kind: 'move', id: item.id, edge: item.edge,
      before: offset < 0 ? peers[index - 1].id : peers[index + 2]?.id});
    void this.updateComplete.then(() => this.focusSelected());
  }
  focusSelected() {
    const details = this.shadowRoot.querySelector<HTMLDetailsElement>('#individualControls');
    if (details) details.open = true;
    this.shadowRoot.querySelector<HTMLElement>(`[data-control="${CSS.escape(this.selected)}"]`)?.focus();
  }
  preset(name: string) {
    const layout = this.presets[name];
    if (layout) this.act({kind: 'preset', layout});
  }
  onCancelClick() {
    if (this.applying) return;
    if (this.latest_) this.draft = createDraft(this.latest_.layout);
    this.conflict = false;
    this.saveFailed = false;
    this.message = 'Current layout loaded.';
  }
  async onReloadCurrentClick() {
    if (this.applying || this.reloading) return;
    this.reloading = true;
    try {
      let state = await sendWithPromise<LayoutState>('noventraGetLayout');
      if (!this.isConnected) return;
      if (this.latest_ && this.latest_.layout.revision > state.layout.revision) {
        state = this.latest_;
      }
      this.draft = createDraft(state.layout);
      this.acceptState_(state);
      this.conflict = false;
      this.saveFailed = false;
      this.message = 'Current layout loaded.';
    } catch {
      this.message = 'The current layout could not be loaded. Your unsaved choice is kept. Try again.';
    } finally {
      this.reloading = false;
      if (!this.dirty() && !this.conflict && !this.saveFailed) {
        this.resumeNavigation_();
      }
    }
  }
  onRetrySaveClick() {
    if (this.controlsDisabled()) return;
    this.saveFailed = false;
    void this.saveLayout_();
  }
  async whenSaved(): Promise<void> {
    while (this.savePromise_) await this.savePromise_;
  }
  private saveLayout_(): Promise<void> {
    if (this.savePromise_) return this.savePromise_;
    if (!this.draft || !this.dirty() || this.controlsDisabled() || this.saveFailed) {
      return Promise.resolve();
    }
    this.applying = true;
    this.savePromise_ = this.flushChanges_().finally(() => {
      this.applying = false;
      this.savePromise_ = null;
      if (this.deferredState_) {
        const state = this.deferredState_;
        this.deferredState_ = null;
        this.acceptState_(state);
      }
      if (this.isConnected && this.dirty() && !this.controlsDisabled() && !this.saveFailed) {
        void this.saveLayout_();
      } else if (this.isConnected && this.pendingNavigation) {
        if (!this.dirty() && !this.conflict) {
          this.resumeNavigation_();
        } else {
          void this.showNavigationError_();
        }
      }
    });
    return this.savePromise_;
  }
  private async flushChanges_(): Promise<void> {
    while (this.isConnected && this.draft && this.dirty() &&
           !this.controlsDisabled() && !this.saveFailed) {
      const submitted = structuredClone(this.draft.current);
      try {
        const result = await sendWithPromise<ApplyResult>('noventraApplyLayout', {
          expectedRevision: this.draft.saved.revision, layout: submitted,
        });
        if (result.status !== 'ok') {
          this.conflict = result.status === 'conflict';
          this.saveFailed = true;
          this.message = result.message || 'The layout could not be saved. Retry or reload the current layout.';
          break;
        }
        this.draft = acknowledgeSave(this.draft, submitted, result.layout);
        if (this.latest_) this.latest_ = {...this.latest_, layout: result.layout};
        this.message = this.dirty() ? 'Saving your latest layout choice…' : 'Layout saved.';
        if (this.deferredState_) {
          const state = this.deferredState_;
          this.deferredState_ = null;
          this.acceptState_(state);
        }
      } catch {
        this.saveFailed = true;
        this.message = 'The layout could not be saved. Retry or reload the current layout.';
      }
    }
    if (this.saveFailed || this.conflict) {
      try {
        const state = await sendWithPromise<LayoutState>('noventraGetLayout');
        if (this.isConnected) this.receive(state);
      } catch { /* The unsaved choice stays available for an explicit retry. */ }
    }
  }
  private cancelPointer_() {
    if (this.scrollFrame_ !== null) cancelAnimationFrame(this.scrollFrame_);
    this.scrollFrame_ = null; this.scrollTime_ = 0;
    if (this.pointer_ && this.hasPointerCapture(this.pointer_.id)) this.releasePointerCapture(this.pointer_.id);
    this.pointer_ = null; this.dropTarget = null; this.dragGhost = null;
  }
  private readonly onPointerCancel_ = () => this.cancelPointer_();
  private readonly onPointerMove_ = (event: PointerEvent) => {
    const pointer = this.pointer_;
    if (!pointer || pointer.id !== event.pointerId) return;
    if (!pointer.moving && Math.hypot(event.clientX - pointer.x, event.clientY - pointer.y) < 6) return;
    pointer.moving = true;
    pointer.currentX = event.clientX; pointer.currentY = event.clientY;
    this.dragGhost = {id: pointer.control, x: Math.min(event.clientX + 14, innerWidth - 170),
      y: Math.min(event.clientY + 14, innerHeight - 40)};
    event.preventDefault();
    this.updateDropTarget_(event.clientX, event.clientY);
    if (this.scrollFrame_ === null) this.scrollFrame_ = requestAnimationFrame(this.autoScroll_);
  };
  private updateDropTarget_(x: number, y: number) {
    const hit = this.shadowRoot.elementFromPoint(x, y);
    const zone = hit?.closest<HTMLElement>('[data-edge]');
    if (zone) {
      const edge = zone.dataset['edge'] as DockEdge;
      const chips = Array.from(zone.querySelectorAll<HTMLElement>('[data-control]')).filter(chip =>
        chip.dataset['control'] !== 'spaces');
      const rects = chips.map(chip => {
        const rect = chip.getBoundingClientRect();
        return {id: chip.dataset['control']!, x: rect.x, y: rect.y, width: rect.width, height: rect.height};
      });
      const before = this.selected === 'spaces' ? undefined :
          dropBefore(rects, x, y, getComputedStyle(this).direction === 'rtl');
      this.dropTarget = this.canPlace(edge) ? {edge, before} : null;
    } else {
      this.dropTarget = hit?.closest('.tray') ? {edge: 'tray'} : null;
    }
  }
  private readonly autoScroll_ = (time: number) => {
    this.scrollFrame_ = null;
    const pointer = this.pointer_;
    if (!pointer?.moving) { this.scrollTime_ = 0; return; }
    const interval = this.scrollTime_ ? Math.min(32, time - this.scrollTime_) : 16;
    this.scrollTime_ = time;
    // Settings scrolls an ancestor inside a shadow root, not necessarily the
    // document. Walk the composed tree and scroll the closest usable container.
    let node: HTMLElement|null = this;
    while (node) {
      const root = node.getRootNode();
      if (node.scrollHeight > node.clientHeight + 1 &&
          (/(auto|scroll)/.test(getComputedStyle(node).overflowY) || node === document.scrollingElement)) {
        const rect = node.getBoundingClientRect();
        const top = Math.max(0, rect.top), bottom = Math.min(innerHeight, rect.bottom);
        const margin = Math.min(48, (bottom - top) / 3);
        const speed = pointer.currentY < top + margin ? -Math.min(1, (top + margin - pointer.currentY) / margin) :
            pointer.currentY > bottom - margin ? Math.min(1, (pointer.currentY - bottom + margin) / margin) : 0;
        if (speed && (speed < 0 ? node.scrollTop > 0 : node.scrollTop < node.scrollHeight - node.clientHeight - 1)) {
          node.scrollTop += speed * interval * .6;
          this.updateDropTarget_(pointer.currentX, pointer.currentY);
          this.scrollFrame_ = requestAnimationFrame(this.autoScroll_);
          return;
        }
      }
      node = node.parentElement || (root instanceof ShadowRoot ? root.host as HTMLElement : null);
    }
    this.scrollTime_ = 0;
  };
  private readonly onEditorKeydown_ = (event: KeyboardEvent) => {
    if (event.key === 'Escape' && this.pointer_) {
      event.preventDefault(); event.stopPropagation(); this.cancelPointer_();
      this.message = 'Drag cancelled.';
      return;
    }
    const target = event.composedPath()[0] as HTMLElement;
    if (target.matches('input,textarea') || target.isContentEditable || this.controlsDisabled()) return;
    if ((event.ctrlKey || event.metaKey) && !event.altKey && event.key.toLowerCase() === 'z') {
      event.preventDefault(); this.act({kind: event.shiftKey ? 'redo' : 'undo'});
      void this.updateComplete.then(() => this.focusSelected());
    }
  };
  private readonly onPointerUp_ = (event: PointerEvent) => {
    const pointer = this.pointer_;
    if (!pointer || pointer.id !== event.pointerId) return;
    // Revealed drop targets and scrolling can change the hit after the last
    // pointer move. Use the actual release position before committing.
    if (pointer.moving) this.updateDropTarget_(event.clientX, event.clientY);
    const target = this.dropTarget;
    if (pointer.moving && target) {
      if (target.edge === 'tray') this.act({kind: 'hide', id: pointer.control});
      else this.act({kind: 'move', id: pointer.control, edge: target.edge, before: target.before});
    }
    this.cancelPointer_();
    void this.updateComplete.then(() => this.focusSelected());
  };
  chipKey(event: KeyboardEvent, id: string) {
    this.selected = id;
    if (event.altKey && ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(event.key)) {
      const edge = keyboardEdge(event.key);
      if (event.shiftKey && !edge) return;
      event.preventDefault(); event.stopPropagation();
      if (event.shiftKey && edge) this.move(edge);
      else this.reorder(event.key === 'ArrowUp' || event.key === 'ArrowLeft' ? -1 : 1);
      void this.updateComplete.then(() => this.focusSelected());
    } else if (event.key === 'Delete') {
      event.preventDefault(); this.act({kind: 'hide', id});
      void this.updateComplete.then(() => this.focusSelected());
    }
  }
  private readonly beforeUnload_ = (event: BeforeUnloadEvent) => {
    if (this.dirty() || this.applying) { event.preventDefault(); event.returnValue = ''; }
  };
  private readonly beforeNavigation_ = (event: Event) => {
    if ((!this.dirty() && !this.applying) || !this.isConnected) return;
    event.preventDefault();
    this.navigationFocus_ = this.shadowRoot.activeElement as HTMLElement|null;
    this.pendingNavigation = (event as CustomEvent<{resume: () => void}>).detail.resume;
    if (this.saveFailed || this.conflict) {
      void this.showNavigationError_();
    } else {
      this.message = 'Saving layout before leaving…';
      void this.saveLayout_();
    }
  };
  private async showNavigationError_() {
    const visibility = {complete: Promise.resolve<unknown>(undefined)};
    this.fire('noventra-layout-request-visibility', visibility);
    await Promise.all([visibility.complete, this.updateComplete]);
    if (this.isConnected && this.pendingNavigation) {
      const dialog = this.shadowRoot.querySelector<HTMLDialogElement>('dialog');
      if (dialog && !dialog.open) dialog.showModal();
    }
  }
  private resumeNavigation_() {
    if (!this.isConnected || !this.pendingNavigation) return;
    const resume = this.pendingNavigation;
    this.pendingNavigation = null;
    this.shadowRoot.querySelector<HTMLDialogElement>('dialog')?.close();
    resume();
  }
  onKeepEditingClick() {
    this.shadowRoot.querySelector<HTMLDialogElement>('dialog')?.close();
    this.pendingNavigation = null; this.navigationFocus_?.focus();
  }
  onDiscardAndLeaveClick() {
    const resume = this.pendingNavigation;
    this.onCancelClick(); this.onKeepEditingClick(); resume?.();
  }

}
customElements.define(NoventraLayoutEditorElement.is, NoventraLayoutEditorElement);

declare global {
  interface HTMLElementTagNameMap { 'noventra-layout-editor': NoventraLayoutEditorElement; }
}
