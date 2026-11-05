// Copyright 2025 The Helium Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

import '../controls/settings_dropdown_menu.js';
import '../controls/settings_toggle_button.js';
import '../settings_page/settings_subpage.js';
import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import 'chrome://resources/cr_elements/cr_checkbox/cr_checkbox.js';
import 'chrome://resources/cr_elements/cr_collapse/cr_collapse.js';
import 'chrome://resources/cr_elements/cr_icon/cr_icon.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.js';
import 'chrome://resources/cr_elements/cr_link_row/cr_link_row.js';

import {PrefsMixin} from '/shared/settings/prefs/prefs_mixin.js';
import {routes} from '../route.js';
import {Router} from '../router.js';
import {HelpBubbleMixin} from 'chrome://resources/cr_components/help_bubble/help_bubble_mixin.js';
import type {CrInputElement} from 'chrome://resources/cr_elements/cr_input/cr_input.js';
import {I18nMixin} from 'chrome://resources/cr_elements/i18n_mixin.js';
import {WebUiListenerMixin} from 'chrome://resources/cr_elements/web_ui_listener_mixin.js';
import {sendWithPromise} from 'chrome://resources/js/cr.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {PolymerElement} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js';

import type {
  DropdownMenuOptionList,
  SettingsDropdownMenuElement,
} from '../controls/settings_dropdown_menu.js';
import type {HeliumServicesChangeHandler} from './changelog_proxy.js';
import {HeliumServicesChangeHandlerImpl} from './changelog_proxy.js';
import {getTemplate} from './services_page.html.js';

const SettingsHeliumServicesPageElementBase =
    HelpBubbleMixin(WebUiListenerMixin(
        I18nMixin(PrefsMixin(PolymerElement))));

enum CrashReportingMode {
  DISABLED = -1,
  ASK = 0,
  AUTOMATIC = 1,
}

interface HeliumCrashReportingState {
  mode: CrashReportingMode;
  managed: boolean;
  configured: boolean;
}

export class SettingsHeliumServicesPageElement extends
    SettingsHeliumServicesPageElementBase {
  private changeProxy_: HeliumServicesChangeHandler =
      HeliumServicesChangeHandlerImpl.getInstance();

  static get is() {
    return 'settings-helium-services-page';
  }

  static get template() {
    return getTemplate();
  }

  static get properties() {
    return {
      ignoreChecked_: Boolean,
      crashReportingConfigured_: {type: Boolean, value: false},
      changes_: Array,
      crashReportingOptions_: {
        type: Array,
        readOnly: true,
        value: () => [
          {
            value: CrashReportingMode.DISABLED,
            name: loadTimeData.getString('heliumCrashReportingDisabled'),
          },
          {
            value: CrashReportingMode.ASK,
            name: loadTimeData.getString('heliumCrashReportingAsk'),
          },
          {
            value: CrashReportingMode.AUTOMATIC,
            name: loadTimeData.getString('heliumCrashReportingAutomatic'),
          },
        ],
      },
      crashReportingPref_: {
        type: Object,
        value: () => ({
          key: '',
          type: chrome.settingsPrivate.PrefType.NUMBER,
          value: CrashReportingMode.ASK,
        }),
      },
    };
  }

  declare private changes_: string[];
  declare private crashReportingOptions_: DropdownMenuOptionList;
  declare private crashReportingPref_:
      chrome.settingsPrivate.PrefObject<number>;
  declare private ignoreChecked_: boolean;
  declare private crashReportingConfigured_: boolean;

  private toOrigin(thing: string) {
    try {
      return new URL(thing).origin;
    } catch {
      return null;
    }
  }

  private updateChangelogState(changes: string[]) {
    this.changes_ = changes.flatMap(ch => ch.split('\n'));
  }

  private requestChangelog() {
    this.changeProxy_.requestChangelog().then(
        this.updateChangelogState.bind(this));
  }

  private onAcknowledgedChangelog_() {
    this.changeProxy_.acknowledgeChanges(this.ignoreChecked_);
    this.requestChangelog();
  }

  override ready() {
    super.ready();
    this.requestChangelog();
    const update = (state: HeliumCrashReportingState) =>
        this.setCrashReportingState_(state);
    this.addWebUiListener('helium-crash-reporting-change', update);
    sendWithPromise<HeliumCrashReportingState>('getHeliumCrashReporting')
        .then(update);
  }

  private setCrashReportingState_(state: HeliumCrashReportingState) {
    const pref: chrome.settingsPrivate.PrefObject<number> = {
      key: '',
      type: chrome.settingsPrivate.PrefType.NUMBER,
      value: state.mode,
    };
    if (state.managed) {
      pref.enforcement = chrome.settingsPrivate.Enforcement.ENFORCED;
      pref.controlledBy = chrome.settingsPrivate.ControlledBy.USER_POLICY;
    }
    this.crashReportingPref_ = pref;
    this.crashReportingConfigured_ = state.configured;
  }

  private onCrashReportingChange_(event: Event) {
    const control = event.target as SettingsDropdownMenuElement;
    const mode = Number.parseInt(control.getSelectedValue(), 10);
    chrome.send('setHeliumCrashReportingMode', [mode]);
  }

  protected onOriginOverrideKeydown_(event: KeyboardEvent) {
    const target = event.target as CrInputElement;

    if (event.key === 'Escape' || event.key === 'Enter') {
      target.blur();
    }
  }

  private onOriginOverrideChange_(event: Event) {
    const target = event.target as CrInputElement;
    if (target.invalid) {
      return;
    }

    const origin = this.toOrigin(target.value);
    if (origin) {
      target.value = origin;
    }

    this.setPrefValue('helium.services.origin_override', target.value);
  }

  private onSpellingLanguagesClick_() {
    Router.getInstance().navigateTo(routes.LANGUAGES);
  }
}

declare global {
    interface HTMLElementTagNameMap {
      'settings-helium-services-page': SettingsHeliumServicesPageElement;
    }
}

customElements.define(SettingsHeliumServicesPageElement.is, SettingsHeliumServicesPageElement);
