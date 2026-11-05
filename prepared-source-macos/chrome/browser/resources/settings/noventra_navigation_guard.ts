// Copyright 2026 The Noventra Authors
// SPDX-License-Identifier: GPL-3.0-only
// Keep Settings' router authoritative. The editor only vetoes a transition;
// its discard callback resumes the original operation without changing history.
import type {NoventraLayoutEditorElement} from './appearance_page/noventra_layout_editor.js';

export function hasPendingLayoutChanges(root: Document|ShadowRoot = document): boolean {
  for (const element of root.querySelectorAll<HTMLElement>('*')) {
    if (element.localName === 'noventra-layout-editor') {
      const editor = element as NoventraLayoutEditorElement;
      if (editor.dirty() || editor.applying) return true;
    }
    if (element.shadowRoot && hasPendingLayoutChanges(element.shadowRoot)) return true;
  }
  return false;
}

export function allowSettingsNavigation(resume: () => void): boolean {
  return window.dispatchEvent(new CustomEvent('noventra-before-settings-navigation', {
    cancelable: true, detail: {resume},
  }));
}
function entryIndex(): number {
  return (window as Window & {navigation: {currentEntry: {index: number}}})
      .navigation.currentEntry.index;
}
let acceptedIndex = entryIndex();
let restoring = false;
let resumeAfterRestore: (() => void)|null = null;
export function recordSettingsEntry() { acceptedIndex = entryIndex(); }
export function handleSettingsPopState(commit: () => void) {
  if (restoring) {
    restoring = false;
    const resume = resumeAfterRestore;
    resumeAfterRestore = null;
    resume?.();
    return;
  }
  const target = entryIndex();
  const delta = target - acceptedIndex;
  const resume = () => {
    if (restoring) resumeAfterRestore = () => history.go(delta);
    else history.go(delta);
  };
  if (delta && !allowSettingsNavigation(resume)) {
    restoring = true;
    history.go(-delta);
    return;
  }
  acceptedIndex = target;
  commit();
}
