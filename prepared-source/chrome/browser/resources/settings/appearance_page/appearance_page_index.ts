// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://resources/cr_elements/cr_view_manager/cr_view_manager.js';
import './appearance_page.js';

import type {CrViewManagerElement} from 'chrome://resources/cr_elements/cr_view_manager/cr_view_manager.js';
import {CrLitElement} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {routes} from '../route.js';
import type {Route, SettingsRoutes} from '../router.js';
import {RouteObserverMixinLit} from '../router.js';
import type {SearchResult} from '../search_settings.js';
import type {SettingsPlugin} from '../settings_main/settings_plugin.js';
import {SearchableViewContainerMixinLit} from '../settings_page/searchable_view_container_mixin_lit.js';

import {getCss} from './appearance_page_index.css.js';
import {getHtml} from './appearance_page_index.html.js';
import type {SettingsAppearancePageElement} from './appearance_page.js';


export interface SettingsAppearancePageIndexElement {
  $: {
    viewManager: CrViewManagerElement,
    appearance: SettingsAppearancePageElement,
    dataDevices: SettingsAppearancePageElement,
  };
}

const SettingsAppearancePageIndexElementBase =
    SearchableViewContainerMixinLit(RouteObserverMixinLit(CrLitElement));

export class SettingsAppearancePageIndexElement extends
    SettingsAppearancePageIndexElementBase implements SettingsPlugin {
  static get is() {
    return 'settings-appearance-page-index';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      routes_: {
        type: Object,
      },
    };
  }

  protected accessor routes_: SettingsRoutes = routes;

  private containingView_: HTMLElement|null = null;
  private readonly onContainingViewChanged_ = (event: Event) => {
    if (event.target === this.containingView_) {
      this.$.dataDevices.refreshVisibleWorkspaceTools();
    }
  };

  override connectedCallback() {
    super.connectedCallback();
    // The outer Settings view can hide this plugin while its selected inner
    // view stays active. Resume visible tools when that outer view re-enters.
    this.containingView_ = this.closest<HTMLElement>('[slot=view]');
    this.containingView_?.addEventListener(
        'view-enter-finish', this.onContainingViewChanged_);
    this.containingView_?.addEventListener(
        'view-exit-finish', this.onContainingViewChanged_);
  }

  override disconnectedCallback() {
    this.containingView_?.removeEventListener(
        'view-enter-finish', this.onContainingViewChanged_);
    this.containingView_?.removeEventListener(
        'view-exit-finish', this.onContainingViewChanged_);
    this.containingView_ = null;
    super.disconnectedCallback();
  }

  private showDefaultViews_() {
    this.$.viewManager.switchView(
        'appearance', 'no-animation', 'no-animation');
  }

  override async searchContents(query: string): Promise<SearchResult> {
    // Reveal every section before the shared search walker visits it. Keep
    // Fonts as a child view so its matches still highlight the Fonts link.
    this.$.appearance.noventraSearchMode = query.trim().length > 0;
    this.$.dataDevices.noventraSearchMode = query.trim().length > 0;
    await Promise.all([
      this.$.appearance.updateComplete, this.$.dataDevices.updateComplete,
    ]);
    return super.searchContents(query);
  }

  override currentRouteChanged(newRoute: Route, oldRoute?: Route) {
    super.currentRouteChanged(newRoute, oldRoute);

    // Need to wait for currentRouteChanged observers on child views to run
    // first, before switching views.
    queueMicrotask(() => {
      switch (newRoute) {
        case routes.APPEARANCE:
          this.showDefaultViews_();
          break;
        case routes.DATA_DEVICES:
          this.$.viewManager.switchView(
              'dataDevices', 'no-animation', 'no-animation');
          break;
        case routes.FONTS:
          this.$.viewManager.switchView(
              'fonts', 'no-animation', 'no-animation');
          break;
        case routes.BASIC:
          // Switch back to the default view in case they are part of search
          // results.
          this.showDefaultViews_();
          break;
        default:
          // Nothing to do. Other parent elements are responsible for updating
          // the displayed contents.
          break;
      }
    });
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'settings-appearance-page-index': SettingsAppearancePageIndexElement;
  }
}

customElements.define(
    SettingsAppearancePageIndexElement.is, SettingsAppearancePageIndexElement);
