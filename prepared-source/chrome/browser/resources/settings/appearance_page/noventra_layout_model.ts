// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
export type DockEdge = 'top'|'left'|'right';
export interface LayoutItem {id: string; edge: DockEdge; order: number; visible: boolean;}
export interface BrowserLayout {version: number; revision: number; items: LayoutItem[];}
export interface LayoutCapability {
  id: string; label: string; edges: DockEdge[]; hideable: boolean; managed: boolean;
  requiredVisibility?: boolean; restriction?: string;
  members?: Array<{id: string; label: string; independent: boolean}>;
}
export interface LayoutDraft {
  saved: BrowserLayout; current: BrowserLayout; undo: BrowserLayout[]; redo: BrowserLayout[];
}
export type DraftAction = {kind: 'move'; id: string; edge: DockEdge; before?: string}|
    {kind: 'navigation'; placement: NavigationPlacement}|
    {kind: 'hide'; id: string}|{kind: 'preset'; layout: BrowserLayout}|
    {kind: 'undo'}|{kind: 'redo'}|{kind: 'cancel'};
export const EDGES: DockEdge[] = ['top', 'left', 'right'];
export type NavigationPlacement = 'top'|'sidebar';
export const NAVIGATION_CONTROLS = ['back', 'forward', 'reload', 'address', 'extensions', 'menu'];
export function navigationEdge(layout: BrowserLayout, placement: NavigationPlacement): DockEdge|undefined {
  if (placement === 'top') return 'top';
  const tabs = layout.items.find(item => item.id === 'tabs' && item.visible);
  return placement === 'sidebar' && tabs && (tabs.edge === 'left' || tabs.edge === 'right') ? tabs.edge : undefined;
}
export function canPlaceNavigation(layout: BrowserLayout, caps: LayoutCapability[], placement: NavigationPlacement): boolean {
  const edge = navigationEdge(layout, placement);
  const controls = layout.items.filter(item => item.visible && NAVIGATION_CONTROLS.includes(item.id));
  return !!edge && !!controls.length && !caps.some(cap => cap.managed) && controls.every(item => {
    const cap = caps.find(cap => cap.id === item.id);
    return !!cap && cap.edges.includes(edge) && cap.requiredVisibility !== false;
  });
}
export function navigationMatches(layout: BrowserLayout, placement: NavigationPlacement): boolean {
  const edge = navigationEdge(layout, placement);
  const controls = layout.items.filter(item => item.visible && NAVIGATION_CONTROLS.includes(item.id));
  return !!edge && !!controls.length && controls.every(item => item.edge === edge);
}
export function keyboardEdge(key: string): DockEdge|undefined {
  switch (key) {
    case 'ArrowUp': return 'top';
    case 'ArrowLeft': return 'left';
    case 'ArrowRight': return 'right';
    default: return undefined;
  }
}
// Native side docks reserve Spaces for their footer regardless of saved order.
export function visibleItems(layout: BrowserLayout, edge: DockEdge): LayoutItem[] {
  return layout.items.filter(item => item.visible && item.edge === edge)
      .sort((a, b) => Number(a.id === 'spaces') - Number(b.id === 'spaces') || a.order - b.order);
}
// A compact workspace uses the existing native docks and controls. Keep the
// complete capability set so adding a preset never drops an installed tool.
export function workspacePreset(base: BrowserLayout): BrowserLayout {
  const layout = structuredClone(base);
  const top = ['back', 'forward', 'reload', 'address', 'extensions', 'menu'];
  const left = ['action:bookmarks', 'new-tab', 'tabs', 'spaces'];
  for (const item of layout.items) {
    const index = top.indexOf(item.id);
    const sideIndex = left.indexOf(item.id);
    item.visible = index >= 0 || sideIndex >= 0;
    if (index >= 0) { item.edge = 'top'; item.order = index; }
    else if (sideIndex >= 0) { item.edge = 'left'; item.order = sideIndex; }
    else if (!EDGES.includes(item.edge)) item.edge = 'top';
  }
  for (const edge of EDGES) {
    layout.items.filter(item => item.edge === edge)
        .sort((a, b) => Number(b.visible) - Number(a.visible) || a.order - b.order)
        .forEach((item, order) => item.order = order);
  }
  return layout;
}
export interface DropRect {id: string; x: number; y: number; width: number; height: number;}
// Use rendered rows, not the zone's nominal orientation: side buttons wrap,
// regions span a row, and a narrow preview can put icons in a single column.
export function dropBefore(rects: DropRect[], x: number, y: number, rtl = false): string|undefined {
  const rows: DropRect[][] = [];
  for (const rect of rects) {
    const row = rows.at(-1);
    // Centered controls can share a row without sharing their top coordinate.
    // Native-looking spaces and icon buttons have different heights.
    if (row && rect.y < Math.max(...row.map(r => r.y + r.height)) &&
        rect.y + rect.height > Math.min(...row.map(r => r.y))) row.push(rect);
    else rows.push([rect]);
  }
  for (let index = 0; index < rows.length; ++index) {
    const row = rows[index];
    const top = Math.min(...row.map(rect => rect.y));
    const bottom = Math.max(...row.map(rect => rect.y + rect.height));
    const next = rows[index + 1];
    if (y > (next ? (bottom + Math.min(...next.map(rect => rect.y))) / 2 : bottom)) continue;
    if (y < top) return row[0].id;
    if (row.length === 1) return y <= row[0].y + row[0].height / 2 ? row[0].id : next?.[0].id;
    return row.find(rect => rtl ? x >= rect.x + rect.width / 2 : x <= rect.x + rect.width / 2)?.id || next?.[0].id;
  }
  return undefined;
}
export function createDraft(layout: BrowserLayout): LayoutDraft {
  return {saved: structuredClone(layout), current: structuredClone(layout), undo: [], redo: []};
}
function signature(layout: BrowserLayout): string {
  return JSON.stringify([
    ...EDGES.map(edge => visibleItems(layout, edge).map(i => i.id)),
    layout.items.filter(i => !i.visible).map(i => i.id).sort(),
  ]);
}
export function isDirty(state: LayoutDraft): boolean {
  return signature(state.saved) !== signature(state.current);
}
// Acknowledging an automatic save must not discard choices made while it was
// in flight, or the undo/redo history of changes already saved successfully.
export function acknowledgeSave(state: LayoutDraft, submitted: BrowserLayout,
                                saved: BrowserLayout): LayoutDraft {
  const rebase = (layout: BrowserLayout): BrowserLayout =>
      ({...structuredClone(layout), revision: saved.revision});
  return {
    saved: structuredClone(saved),
    current: signature(state.current) === signature(submitted) ?
        structuredClone(saved) : rebase(state.current),
    undo: state.undo.map(rebase), redo: state.redo.map(rebase),
  };
}
function validate(layout: BrowserLayout, caps: LayoutCapability[], previous: BrowserLayout) {
  if (layout.items.length !== caps.length || new Set(layout.items.map(i => i.id)).size !== caps.length) {
    throw new Error('Available controls changed. Reload the saved layout.');
  }
  for (const item of layout.items) {
    const cap = caps.find(c => c.id === item.id);
    if (!EDGES.includes(item.edge) || !cap || !cap.edges.includes(item.edge)) {
      throw new Error('This placement is not available.');
    }
    if (!cap.hideable && !item.visible) throw new Error(`${cap.label} must stay visible.`);
    if (cap.requiredVisibility !== undefined && item.visible !== cap.requiredVisibility) {
      throw new Error(`${cap.label} visibility is managed by your organization.`);
    }
    if (cap.managed && signature({ ...layout, items: [item]}) !==
        signature({...previous, items: previous.items.filter(i => i.id === item.id)})) {
      throw new Error('Your organization controls this layout.');
    }
  }
  // A managed layout also prevents reordering, which a single-item signature cannot detect.
  if (caps.some(c => c.managed) && signature(layout) !== signature(previous)) {
    throw new Error('Your organization controls this layout.');
  }
}
export function reduceDraft(state: LayoutDraft, action: DraftAction, caps: LayoutCapability[]): LayoutDraft {
  if (action.kind === 'cancel') return createDraft(state.saved);
  if (action.kind === 'undo' || action.kind === 'redo') {
    const from = action.kind === 'undo' ? state.undo : state.redo;
    const target = from.at(-1);
    if (!target) return state;
    validate(target, caps, state.current);
    return action.kind === 'undo' ? {...state, current: structuredClone(target),
      undo: from.slice(0, -1), redo: [...state.redo, state.current]} :
      {...state, current: structuredClone(target), redo: from.slice(0, -1), undo: [...state.undo, state.current]};
  }
  let next = structuredClone(state.current);
  if (action.kind === 'preset') {
    next = structuredClone(action.layout);
    next.revision = state.saved.revision;
    // Presets honor current policy without persisting a policy override.
    for (const item of next.items) {
      const cap = caps.find(c => c.id === item.id);
      if (cap?.requiredVisibility !== undefined) item.visible = cap.requiredVisibility;
    }
  } else if (action.kind === 'navigation') {
    if (!canPlaceNavigation(next, caps, action.placement)) {
      throw new Error('This navigation placement is not available. Check the tabs position and organization restrictions.');
    }
    const edge = navigationEdge(next, action.placement)!;
    let restoredTopTools = false;
    if (action.placement === 'top') {
      // Choosing Top creates a usable toolbar, including when the sidebar
      // preset hid address entry. Keep individually hidden navigation buttons
      // and organization restrictions intact.
      for (const item of next.items) {
        const cap = caps.find(capability => capability.id === item.id);
        if (!item.visible && ['address', 'extensions', 'menu'].includes(item.id) &&
            cap?.requiredVisibility !== false && cap?.edges.includes(edge)) {
          item.visible = true;
          restoredTopTools = true;
        }
      }
    }
    const moving = EDGES.filter(source => source !== edge).flatMap(source =>
      visibleItems(next, source).filter(item => NAVIGATION_CONTROLS.includes(item.id)));
    let peers = visibleItems(next, edge).filter(item => item.id !== 'spaces');
    if (action.placement === 'sidebar') {
      // Explicitly choosing this arrangement also repairs an older all-sidebar
      // draft. Keep custom order within navigation and among unrelated tools.
      // Share the header row, with the address field underneath it.
      const controls = [...peers.filter(item => NAVIGATION_CONTROLS.includes(item.id)), ...moving];
      const navigation = controls.filter(item => !['address', 'extensions', 'menu'].includes(item.id));
      const bookmarks = peers.filter(item => item.id === 'action:bookmarks');
      const extensions = controls.filter(item => item.id === 'extensions');
      const menu = controls.filter(item => item.id === 'menu');
      const address = controls.filter(item => item.id === 'address');
      peers = peers.filter(item => !NAVIGATION_CONTROLS.includes(item.id) && item.id !== 'action:bookmarks');
      const firstTabControl = peers.findIndex(item => ['new-tab', 'tabs'].includes(item.id));
      peers.splice(firstTabControl, 0, ...navigation, ...bookmarks, ...extensions, ...menu, ...address);
    } else {
      if (!moving.length && !restoredTopTools) return state;
      // Put restored controls in the same row as existing navigation. Keep
      // its custom button order, with address entry before the browser tools.
      const controls = [...peers.filter(item => NAVIGATION_CONTROLS.includes(item.id)), ...moving];
      const navigation = controls.filter(item => !['address', 'extensions', 'menu'].includes(item.id));
      const address = controls.filter(item => item.id === 'address');
      const tools = controls.filter(item => ['extensions', 'menu'].includes(item.id));
      peers = peers.filter(item => !NAVIGATION_CONTROLS.includes(item.id));
      const tabsIndex = peers.findIndex(item => item.id === 'tabs');
      peers.splice(tabsIndex < 0 ? peers.length : tabsIndex, 0, ...navigation, ...address, ...tools);
    }
    for (const item of moving) item.edge = edge;
    peers.forEach((item, order) => item.order = order);
  } else {
    const item = next.items.find(i => i.id === action.id);
    if (!item) throw new Error('This control is no longer available.');
    if (action.kind === 'hide') item.visible = false;
    else {
      if (item.id === 'spaces' && item.visible && item.edge === action.edge) return state;
      if (action.before === action.id && item.visible && item.edge === action.edge) return state;
      const peers = visibleItems(next, action.edge).filter(i => i.id !== item.id && i.id !== 'spaces');
      // Dropping at or below the footer means the end of the movable controls.
      const before = item.id === 'spaces' || action.before === 'spaces' ? undefined : action.before;
      const index = before ? peers.findIndex(i => i.id === before) : peers.length;
      if (index < 0) throw new Error('The drop destination changed. Try again.');
      item.edge = action.edge; item.visible = true;
      peers.splice(index, 0, item);
      peers.forEach((peer, order) => peer.order = order);
    }
  }
  // Visible and hidden items share each edge's unique order namespace.
  for (const edge of EDGES) {
    next.items.filter(i => i.edge === edge).sort((a, b) =>
      Number(b.visible) - Number(a.visible) ||
      Number(a.id === 'spaces') - Number(b.id === 'spaces') || a.order - b.order)
      .forEach((item, order) => item.order = order);
  }
  validate(next, caps, state.current);
  if (signature(next) === signature(state.current)) return state;
  return {...state, current: next, undo: [...state.undo, state.current].slice(-100), redo: []};
}
// Same-revision effective changes come from policy. Keep user placement edits,
// but follow forced visibility and restore personal visibility on policy removal.
export function reconcileDraft(state: LayoutDraft, saved: BrowserLayout, caps: LayoutCapability[]): LayoutDraft {
  const next = structuredClone(state);
  let changed = false;
  for (const item of next.current.items) {
    const prior = state.saved.items.find(i => i.id === item.id);
    const latest = saved.items.find(i => i.id === item.id);
    const cap = caps.find(c => c.id === item.id);
    if (!prior || !latest || !cap) continue;
    const visible = cap.requiredVisibility ?? (item.visible === prior.visible ? latest.visible : item.visible);
    if (item.visible !== visible || prior.visible !== latest.visible) changed = true;
    item.visible = visible;
  }
  if (!changed) return state;
  next.saved = structuredClone(saved);
  next.undo = []; next.redo = [];
  return next;
}
