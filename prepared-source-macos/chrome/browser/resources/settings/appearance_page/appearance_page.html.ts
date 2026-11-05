// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, repeat} from '//resources/lit/v3_0/lit.rollup.js';
// <if expr="not is_chromeos">
import {ColorSchemeMode} from 'chrome://resources/cr_components/customize_color_scheme_mode/customize_color_scheme_mode.mojom-webui.js';
// </if>

import type {SettingsAppearancePageElement} from './appearance_page.js';

export function getHtml(this: SettingsAppearancePageElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<settings-section page-title="${this.dataDevicesPage ? 'Data & devices' : '$i18n{appearancePageTitle}'}">
  <div route-path="default">
    ${this.dataDevicesPage ? html`
    <section id="dataDevicesContent" class="customize-section" tabindex="-1"
        aria-label="Data and devices">
      <noventra-transfer></noventra-transfer>
      <details id="noventraDataAdvanced" class="space-details"
          ?open="${this.noventraSearchMode}" @toggle="${this.onWorkspaceSyncToggle_}">
        <summary>Advanced: space and layout settings</summary>
        <p>For an existing settings file or your own sync server. To move your browser to another PC, use Encrypted file or Direct transfer above.</p>
      <details id="noventraWorkspaceBackup" class="space-details" ?open="${this.noventraSearchMode}">
        <summary>Space and layout settings file (JSON)</summary>
        <p>Save space names, order, archive state, domain rules and your saved layout. Tabs, logins, cookies, passwords and site data are not included. This is a settings file, not automatic device sync.</p>
        <div class="noventra-workspace-tool-row">
          <cr-button id="noventraExportSettings" @click="${this.onExportWorkspaceSettingsClick_}"
              ?disabled="${this.noventraToolsPending_}">Export settings</cr-button>
          <cr-button id="noventraChooseImport" @click="${this.onChooseWorkspaceSettingsClick_}"
              ?disabled="${this.noventraToolsPending_ || !this.noventraImportable_}">Choose settings file</cr-button>
          <input type="file" id="noventraImportFile" accept=".json,application/json" hidden
              @change="${this.onWorkspaceSettingsFileChange_}">
        </div>
        <div id="noventraImportReview" ?hidden="${!this.noventraImportText_}">
          <p class="noventra-rule-name">${this.noventraImportName_}: add ${this.noventraImportCount_} signed-out space copies. Existing spaces and tabs stay unchanged; existing domain rules take priority. Personal keeps its current name.</p>
          <label class="noventra-workspace-tool-row">
            <input id="noventraImportLayout" type="checkbox" .checked="${this.noventraImportLayout_}"
                @change="${this.onImportWorkspaceLayoutChange_}" ?disabled="${this.noventraToolsPending_}">
            Include layout — replace the saved layout with the file's layout
          </label>
          <div class="noventra-workspace-tool-row">
            <cr-button id="noventraConfirmImport" class="action-button"
                @click="${this.onImportWorkspaceSettingsClick_}" ?disabled="${this.noventraToolsPending_}">Import settings</cr-button>
            <cr-button id="noventraCancelImport" @click="${this.onCancelWorkspaceImportClick_}"
                ?disabled="${this.noventraToolsPending_}">Cancel</cr-button>
          </div>
        </div>
      </details>
      <div id="noventraWorkspaceToolsStatus" role="status" class="cr-row secondary"
          ?hidden="${!this.noventraToolsStatus_}">${this.noventraToolsStatus_}</div>
      <div id="noventraWorkspaceToolsError" role="alert" class="cr-row"
          ?hidden="${!this.noventraToolsError_}">${this.noventraToolsError_}</div>
      <details id="noventraWorkspaceSync" class="space-details" ?open="${this.noventraSearchMode}" @toggle="${this.onWorkspaceSyncToggle_}">
        <summary>Self-hosted space and layout sync</summary>
        <p>Sync space names, order, archive state and your saved layout through a server you control. Changes travel encrypted. Tabs, logins, cookies, passwords, history and domain rules stay on this device.</p>
        <div ?hidden="${this.noventraSync_.configured}">
          <label class="noventra-tool-field">Set up sync
            <select id="noventraSyncMode" class="md-select" .value="${this.noventraSyncMode_}"
                @change="${this.onWorkspaceSyncModeChange_}" ?disabled="${this.noventraSyncBusy_}">
              <option value="create">Create a new vault</option>
              <option value="join">Join an existing vault</option>
            </select>
          </label>
          <div class="noventra-workspace-tool-row">
            <label class="noventra-tool-field flex">Server origin
              <input id="noventraSyncOrigin" type="url" class="noventra-space-input"
                  placeholder="https://sync.example.com" autocomplete="off" spellcheck="false"
                  .value="${this.noventraSyncOrigin_}" @input="${this.onWorkspaceSyncFieldInput_}"
                  ?disabled="${this.noventraSyncBusy_}">
            </label>
            <label class="noventra-tool-field flex">Server access token
              <input id="noventraSyncToken" type="password" class="noventra-space-input"
                  autocomplete="off" spellcheck="false" .value="${this.noventraSyncToken_}"
                  @input="${this.onWorkspaceSyncFieldInput_}" ?disabled="${this.noventraSyncBusy_}">
            </label>
          </div>
          <p class="secondary">Use HTTPS for other devices, or a local HTTP server on localhost for testing. Keep the server access token private.</p>
          <div ?hidden="${this.noventraSyncMode_ !== 'join'}">
            <div class="noventra-workspace-tool-row">
              <label class="noventra-tool-field flex">Vault ID
                <input id="noventraSyncVaultId" class="noventra-space-input" autocomplete="off" spellcheck="false"
                    .value="${this.noventraSyncVaultId_}" @input="${this.onWorkspaceSyncFieldInput_}"
                    ?disabled="${this.noventraSyncBusy_}">
              </label>
              <label class="noventra-tool-field flex">Recovery key
                <input id="noventraSyncJoinKey" type="password" class="noventra-space-input"
                    autocomplete="off" spellcheck="false" .value="${this.noventraSyncJoinKey_}"
                    @input="${this.onWorkspaceSyncFieldInput_}" ?disabled="${this.noventraSyncBusy_}">
              </label>
            </div>
            <p>Joining applies the vault's saved layout and adds its spaces with separate local logins. Existing spaces stay local. Personal's name follows the vault. Spaces you create after joining will sync.</p>
          </div>
          <cr-button id="noventraConnectSync" class="action-button" @click="${this.onConnectWorkspaceSyncClick_}"
              ?disabled="${this.noventraSyncBusy_ || !this.noventraSyncAvailable_ || !this.noventraSyncOrigin_.trim() || !this.noventraSyncToken_ || (this.noventraSyncMode_ === 'join' && (!this.noventraSyncVaultId_.trim() || !this.noventraSyncJoinKey_.trim()))}">
            ${this.noventraSyncMode_ === 'join' ? 'Join vault and apply layout' : 'Create sync vault'}
          </cr-button>
        </div>
        <div ?hidden="${!this.noventraSync_.configured}">
          <p class="noventra-rule-name">Server: ${this.noventraSync_.origin}<br>Vault ID: <span id="noventraConnectedVaultId">${this.noventraSync_.vault_id}</span></p>
          <p ?hidden="${!this.noventraSync_.local_only_spaces}">${this.noventraSync_.local_only_spaces === 1 ? '1 existing space stays local to this device.' : `${this.noventraSync_.local_only_spaces} existing spaces stay local to this device.`}</p>
          <div class="noventra-workspace-tool-row">
            <cr-button id="noventraSyncNow" @click="${this.onWorkspaceSyncNowClick_}"
                ?disabled="${this.noventraSyncBusy_ || this.noventraSync_.syncing || this.noventraSync_.conflict}">Sync now</cr-button>
            <cr-button id="noventraDisconnectSync" @click="${this.onDisconnectWorkspaceSyncClick_}"
                ?disabled="${this.noventraSyncDisconnecting_}">Disconnect</cr-button>
          </div>
          <p class="secondary">Disconnecting keeps this device's spaces and the server vault.</p>
        </div>
        <section id="noventraSyncRecovery" ?hidden="${!this.noventraSyncRecovery_}" aria-label="Save your recovery key">
          <p>Save this recovery key somewhere private before closing Settings. You need it to connect another device. Anyone with this key and access to the vault can read its workspace settings.</p>
          <label class="noventra-tool-field">Recovery key
            <input id="noventraSyncRecoveryKey" class="noventra-space-input" readonly
                type="${this.noventraSyncShowRecovery_ ? 'text' : 'password'}" autocomplete="off"
                spellcheck="false" .value="${this.noventraSyncRecovery_}">
          </label>
          <div class="noventra-workspace-tool-row">
            <cr-button id="noventraShowSyncRecovery" @click="${this.onShowWorkspaceRecoveryClick_}">
              ${this.noventraSyncShowRecovery_ ? 'Hide key' : 'Show key'}</cr-button>
            <cr-button id="noventraCopySyncRecovery" @click="${this.onCopyWorkspaceRecoveryClick_}">Copy key</cr-button>
            <cr-button id="noventraSyncRecoverySaved" @click="${this.onSaveWorkspaceRecoveryClick_}">I've saved the key</cr-button>
          </div>
        </section>
        <section id="noventraSyncConflict" ?hidden="${!this.noventraSync_.conflict}" aria-label="Resolve workspace sync conflict">
          <p>Both this device and the vault changed since the last sync. Keep this device's synced workspace settings, or apply the vault's version here. Tabs and logins stay local. A space omitted by the chosen version is kept locally.</p>
          <div class="noventra-workspace-tool-row">
            <cr-button id="noventraSyncUseLocal" data-action="use-local" @click="${this.onResolveWorkspaceSyncClick_}"
                ?disabled="${this.noventraSyncBusy_ || this.noventraSync_.syncing}">Keep this device's version</cr-button>
            <cr-button id="noventraSyncUseRemote" data-action="use-remote" @click="${this.onResolveWorkspaceSyncClick_}"
                ?disabled="${this.noventraSyncBusy_ || this.noventraSync_.syncing}">Use vault version</cr-button>
          </div>
        </section>
        <p id="noventraSyncStatus" role="status" aria-live="polite">${this.workspaceSyncStatus_()}</p>
        <p id="noventraSyncError" role="alert" class="noventra-rule-name"
            ?hidden="${!this.noventraSyncError_ && !this.workspaceSyncErrorMessage_(this.noventraSync_.error)}">${this.noventraSyncError_ || this.workspaceSyncErrorMessage_(this.noventraSync_.error)}</p>
      </details>
      </details>
    </section>` : html`
    <nav class="customize-nav" aria-label="Customization sections"
        no-search ?hidden="${this.noventraSearchMode}">
      <button type="button" data-section="customizeLayout" aria-controls="customizeLayout" aria-current="${this.noventraSection_ === 'customizeLayout' ? 'location' : 'false'}" @click="${this.onNoventraSectionClick_}">Layout</button>
      <button type="button" data-section="customizeAppearance" aria-controls="customizeAppearance" aria-current="${this.noventraSection_ === 'customizeAppearance' ? 'location' : 'false'}" @click="${this.onNoventraSectionClick_}">Appearance</button>
      <button type="button" data-section="customizeSpaces" aria-controls="customizeSpaces" aria-current="${this.noventraSection_ === 'customizeSpaces' ? 'location' : 'false'}" @click="${this.onNoventraSectionClick_}">Spaces</button>
      <button type="button" data-section="customizeBehavior" aria-controls="customizeBehavior" aria-current="${this.noventraSection_ === 'customizeBehavior' ? 'location' : 'false'}" @click="${this.onNoventraSectionClick_}">Tabs & browsing</button>
    </nav>
    <section id="customizeLayout" class="customize-section" tabindex="-1"
        aria-label="Layout" ?hidden="${this.isNoventraSectionHidden_('customizeLayout')}">
      <noventra-layout-editor
          .searchMode="${this.noventraSearchMode}"
          .glassEnabled="${!!this.noventraGlassEnabledPref_?.value}"
          .tabDensity="${this.noventraTabDensityPref_?.value === 1 ? 1 : 0}"
          .bookmarkOpening="${this.noventraBookmarkOpeningPref_?.value === 1 ? 1 : 0}"
          @noventra-layout-request-visibility="${this.onNoventraLayoutRequestVisibility_}">
      </noventra-layout-editor>
      <div id="noventraSidebarPreferences" class="sidebar-preferences">
        <div class="customize-heading"><h2>Sidebar preferences</h2>
          <p>Changes save immediately.</p></div>
        <div class="cr-row">
          <div class="flex"><div>Tab density</div>
            <div class="secondary">Choose compact tabs or more space between them.</div></div>
          <settings-dropdown-menu id="noventraTabDensity"
              pref-key="noventra.browser.tab_density" label="Tab density"
              .menuOptions="${this.noventraDensityOptions_}">
          </settings-dropdown-menu>
        </div>
        <div class="cr-row">
          <div class="flex"><div>Open bookmarks in</div>
            <div class="secondary">Choose a floating popup or a panel beside the page.</div></div>
          <settings-dropdown-menu id="noventraBookmarkOpening"
              pref-key="noventra.browser.bookmark_opening" label="Open bookmarks in"
              .menuOptions="${this.noventraBookmarkOpeningOptions_}">
          </settings-dropdown-menu>
        </div>
      </div>
      <details id="noventraAutoHide" class="space-details settings-options"
          ?open="${this.noventraSearchMode}">
        <summary id="noventraBarVisibilityHeading">Auto-hide browser controls</summary>
      <div id="noventraBarVisibility" tabindex="-1"
          aria-labelledby="noventraBarVisibilityHeading"
          aria-busy="${this.noventraBarVisibilityPending_}">
        <p>Choose which controls appear at the window edge. Changes save immediately.</p>
        <div class="bar-visibility-presets" role="group"
            aria-label="Bar visibility presets">
          ${this.noventraBarVisibilityChoices_.map(choice => html`
            <button type="button" data-visibility="${choice.id}"
                aria-pressed="${this.noventraBarVisibilityMode_() === choice.id}"
                ?disabled="${this.noventraBarVisibilityDisabled_()}"
                @click="${this.onNoventraBarVisibilityClick_}">
              <span class="bar-visibility-preview" aria-hidden="true">
                <span class="bar-visibility-top"></span>
                <span class="bar-visibility-side"></span>
              </span>
              <span>${choice.label}</span>
            </button>
          `)}
        </div>
        <details class="space-details" ?open="${this.noventraSearchMode}">
          <summary>Choose visibility for each bar</summary>
        <div class="bar-visibility-details" role="group"
            aria-label="Individual bar visibility">
          <label for="noventraSidebarVisibility">Sidebars</label>
          <select id="noventraSidebarVisibility" class="md-select"
              ?disabled="${this.noventraBarVisibilityDisabled_()}"
              @change="${this.onNoventraBarVisibilityChange_}">
            <option value="visible">Always visible</option>
            <option value="edge">Reveal at edge</option>
          </select>
          <label for="noventraTopVisibility">Top bar</label>
          <select id="noventraTopVisibility" class="md-select"
              ?disabled="${this.noventraBarVisibilityDisabled_()}"
              @change="${this.onNoventraBarVisibilityChange_}">
            <option value="visible">Always visible</option>
            <option value="edge">Reveal at edge</option>
          </select>
        </div>
        </details>

        <p id="noventraBarVisibilityStatus" role="status">${this.noventraBarVisibilityStatus_()}</p>
        <p id="noventraBarVisibilityError" role="alert"
            ?hidden="${!this.noventraBarVisibilityError_}">${this.noventraBarVisibilityError_}</p>
      </div>
      </details>
    </section>
    <section id="customizeAppearance" class="customize-section" tabindex="-1"
        aria-labelledby="appearanceHeading" ?hidden="${this.isNoventraSectionHidden_('customizeAppearance')}">
      <div class="customize-heading"><h2 id="appearanceHeading">Appearance</h2>
        <p>Your browser's appearance, colors and page display. Changes save immediately.</p></div>
<if expr="not is_chromeos">
    <fieldset id="noventraAppearancePresets"
        ?hidden="${!this.pageVisibility_.setTheme}"
        ?disabled="${this.noventraAppearanceDisabled_()}"
        aria-describedby="noventraAppearancePresetNote"
        aria-busy="${this.noventraAppearancePending_}">
      <legend>Choose a theme</legend>
      <div class="appearance-choices theme-presets">
        ${this.noventraAppearancePresets_.map(preset => html`
          <label class="appearance-choice" data-preset="${preset.id}"
              data-surface="${preset.glass ? 'glass' : 'solid'}">
            <input type="radio" name="noventraThemePreset" value="${preset.id}"
                .checked="${this.isNoventraAppearanceSelected_(preset)}"
                aria-labelledby="noventraPresetName-${preset.id}"
                @change="${this.onNoventraAppearancePresetChange_}">
            <span class="appearance-preview" aria-hidden="true">
              <span class="appearance-preview-pane ${preset.mode === ColorSchemeMode.kDark ? 'dark' : 'light'}"
                  style="--preview-surface: ${preset.preview}">
                <span class="appearance-preview-window">
                  <span class="appearance-preview-toolbar"></span>
                  <span class="appearance-preview-sidebar"></span>
                  <span class="appearance-preview-page"></span>
                </span>
              </span>
            </span>
            <span class="surface-choice-name" id="noventraPresetName-${preset.id}">${preset.name}</span>
            <span class="surface-choice-hint">${preset.glass ? 'Liquid glass' : 'Solid surfaces'}</span>
          </label>`)}
      </div>
      <p id="noventraAppearancePresetNote" class="appearance-note">Themes change browser colors and surface style. Glass needs support from your desktop and falls back to solid when transparency is unavailable.</p>
      <p id="noventraAppearancePresetStatus" class="appearance-note" role="status">${this.noventraAppearanceStatus_()}</p>
      <p id="noventraAppearancePresetError" role="alert"
          ?hidden="${!this.noventraAppearanceError_}">${this.noventraAppearanceError_}</p>
    </fieldset>
</if>
    <details id="noventraCustomAppearance"
        ?hidden="${!this.pageVisibility_.setTheme}"
        ?open="${this.noventraSearchMode}">
      <summary>Custom appearance</summary>
      <p class="appearance-note">Choose surface style, automatic color mode and tint separately.</p>
    <fieldset id="noventraSurfaceChoices"
        ?hidden="${!this.pageVisibility_.setTheme}"
        ?disabled="${this.noventraSurfaceDisabled_()}"
        aria-describedby="noventraSurfaceNote">
      <legend>Surface style</legend>
      <div class="appearance-choices surface-choices">
        ${[{value: 'solid', name: 'Solid', hint: 'Opaque surfaces in any color'},
           {value: 'glass', name: 'Liquid glass', hint: 'Translucent surfaces where supported'}].map(option => html`
          <label class="appearance-choice" data-surface="${option.value}">
            <input type="radio" name="noventraSurface" value="${option.value}"
                .checked="${!!this.noventraGlassEnabledPref_?.value === (option.value === 'glass')}"
                aria-labelledby="noventraSurfaceName-${option.value}"
                aria-describedby="noventraSurfaceHint-${option.value}"
                @change="${this.onNoventraSurfaceChange_}">
            <span class="appearance-preview" aria-hidden="true">
              ${this.noventraSurfacePreviewTones_().map(tone => html`
                <span class="appearance-preview-pane ${tone}">
                  <span class="appearance-preview-window">
                    <span class="appearance-preview-toolbar"></span>
                    <span class="appearance-preview-sidebar"></span>
                    <span class="appearance-preview-page"></span>
                  </span>
                </span>`)}
            </span>
            <span class="surface-choice-name" id="noventraSurfaceName-${option.value}">${option.name}</span>
            <span class="surface-choice-hint" id="noventraSurfaceHint-${option.value}">${option.hint}</span>
          </label>`)}
      </div>
      <p class="appearance-note" id="noventraSurfaceNote">Choose your color mode and tint below. Glass falls back to solid when transparency is unavailable.</p>
    </fieldset>
    <fieldset id="noventraAppearanceChoices"
        ?hidden="${!this.pageVisibility_.setTheme || !this.showColorSchemeMode_()}"
        ?disabled="${this.selectedColorSchemeMode_ === undefined}"
        data-glass="${!!this.noventraGlassEnabledPref_?.value}">
      <legend>Color mode</legend>
      <div class="appearance-choices">
        ${this.colorSchemeModeOptions_.map(option => html`
          <label class="appearance-choice" data-scheme="${option.preview}">
            <input type="radio" name="noventraAppearance" value="${option.value}"
                .checked="${this.isSelectedColorSchemeMode_(option.value)}"
                aria-labelledby="noventraSchemeName-${option.preview}"
                aria-describedby="noventraSchemeHint-${option.preview}"
                @change="${this.onNoventraColorSchemeChange_}">
            <span class="appearance-choice-name" id="noventraSchemeName-${option.preview}">${this.noventraColorSchemeLabel_(option.value)}</span>
            <span class="appearance-choice-hint" id="noventraSchemeHint-${option.preview}">
              ${option.preview === 'system' ? 'Default · follows your device' :
                  option.preview === 'light' ? 'Light browser surfaces' :
                                              'Dark browser surfaces'}
            </span>
          </label>`)}
      </div>
      <!-- Retain the existing programmatic selector without a second UI. -->
      <select id="noventraColorScheme" hidden aria-label="Browser appearance"
          ?disabled="${!this.pageVisibility_.setTheme || !this.showColorSchemeMode_()}"
          @change="${this.onNoventraColorSchemeChange_}">
        ${this.colorSchemeModeOptions_.map(option => html`
          <option value="${option.value}" ?selected="${this.isSelectedColorSchemeMode_(option.value)}">
            ${option.name}
          </option>
        `)}
      </select>
    </fieldset>
    <!-- Keep the existing preference adapter without a second visible control. -->
    <settings-toggle-button id="noventraGlassAppearance" hidden
        ?disabled="${!this.noventraGlassEnabledPref_}"
        pref-key="noventra.browser.glass_enabled"
        label="Glass appearance"
        sub-label="${this.noventraGlassEnabledPref_?.value ?
            'Translucent surfaces where supported. Choose your tint in Colors.' :
            'Solid browser surfaces. Choose your tint in Colors.'}">
    </settings-toggle-button>
<if expr="not is_chromeos">
    <details id="noventraColors" ?hidden="${!this.pageVisibility_.setTheme}"
        ?open="${this.noventraSearchMode || this.noventraColorsOpen_}"
        @toggle="${this.onNoventraColorsToggle_}">
      <summary>Colors</summary>
      <cr-theme-color-picker columns="8"></cr-theme-color-picker>
</if>
    <div class="settings-row first" id="themeRow"
        ?hidden="${!this.pageVisibility_.setTheme}">
      <div class="flex cr-padded-text secondary">${this.themeSublabel_}</div>
<if expr="not is_linux">
      ${this.themeIdPref_?.value ? html`
        <div class="separator"></div>
        <cr-button id="useDefault" @click="${this.onUseDefaultClick_}"
            aria-label="$i18n{resetToDefaultTheme}">
          $i18n{resetToDefault}
        </cr-button>
      ` : ''}
</if>
<if expr="is_linux">
      <div class="settings-row continuation"
          ?hidden="${!this.showUseClassic_()}"
          id="themesSecondaryActions">
        <div class="separator"></div>
        ${this.showUseClassic_() ? html`
          <cr-button id="useDefault" @click="${this.onUseDefaultClick_}">
            $i18n{useClassicTheme}
          </cr-button>
        ` : ''}
      </div>
</if>
    </div>
<if expr="not is_chromeos">
    </details>
</if>
    </details>
    <div class="cr-row">
      <a href="chrome://new-tab-page/?customize=shortcuts" target="_blank">
        New tab background and shortcuts
      </a>
    </div>
    <details id="noventraTabAppearance" class="space-details settings-options"
        ?open="${this.noventraSearchMode}">
      <summary>Tab icons and address bar</summary>
    <settings-toggle-button id="noventraTabLetters" class="hr"
        pref-key="noventra.browser.tab_letters"
        label="Use letters instead of site icons"
        sub-label="Pinned sites always keep their site icons.">
    </settings-toggle-button>
    <div class="list-frame indented-toggles">
      <settings-toggle-button class="hr"
          pref-key="helium.browser.minimal_location_bar"
          label="$i18n{minimalLocationBar}">
      </settings-toggle-button>
      <settings-toggle-button class="hr"
          pref-key="helium.browser.show_progress_bar"
          label="$i18n{showProgressBar}">
      </settings-toggle-button>
    </div>
    </details>
    <details id="noventraPageDisplay" class="space-details settings-options"
        ?open="${this.noventraSearchMode || this.noventraPageDisplayOpen_}"
        @toggle="${this.onNoventraPageDisplayToggle_}">
      <summary>Page text and zoom</summary>
    <div class="cr-row">
      <div class="flex cr-padded-text" aria-hidden="true">
        $i18n{fontSize}
      </div>
      <settings-dropdown-menu id="defaultFontSize" label="$i18n{fontSize}"
          pref-key="webkit.webprefs.default_font_size"
          .menuOptions="${this.fontSizeOptions_}">
      </settings-dropdown-menu>
    </div>
    <cr-link-row class="hr" id="customize-fonts-subpage-trigger"
        label="$i18n{customizeFonts}" @click="${this.onCustomizeFontsClick_}"
        role-description="$i18n{subpageArrowRoleDescription}">
    </cr-link-row>
    <div class="cr-row" ?hidden="${!this.pageVisibility_.pageZoom}">
      <div id="pageZoom" class="flex cr-padded-text" aria-hidden="true">
        $i18n{pageZoom}
      </div>
      <select id="zoomLevel" class="md-select" aria-labelledby="pageZoom"
          @change="${this.onZoomLevelChange_}">
        ${this.pageZoomLevels_.map(item => html`
          <option value="${item}"
              ?selected="${this.zoomValuesEqual_(item, this.defaultZoom_)}">
            ${this.formatZoom_(item)}%
          </option>
        `)}
      </select>
    </div>
    <div class="list-frame indented-toggles"
        ?hidden="${!this.pageVisibility_.pageZoom}">
      <settings-toggle-button id="showZoomIndicator"
          pref-key="helium.browser.show_zoom_indicator"
          label="$i18n{showZoomIndicator}">
      </settings-toggle-button>
    </div>
    </details>
    <details id="noventraBookmarkBarSettings" class="space-details settings-options" ?open="${this.noventraSearchMode}">
      <summary>Horizontal bookmarks bar</summary>
    ${!this.ntpSimplificationBookmarksBarEnabled_ ? html`
      <settings-toggle-button id="showBookmarksBar"
          ?hidden="${!this.pageVisibility_.bookmarksBar}"
          pref-key="bookmark_bar.show_on_all_tabs"
          label="Show horizontal bookmarks bar"
          sub-label="Separate from the Bookmarks button in your sidebar.">
      </settings-toggle-button>
    ` : ''}
    ${this.ntpSimplificationBookmarksBarEnabled_ ? html`
      <div class="cr-row" ?hidden="${!this.pageVisibility_.bookmarksBar}">
        <div class="flex cr-padded-text" aria-hidden="true">
          $i18n{bookmarksBar}
        </div>
        <settings-dropdown-menu id="bookmarksBarVisibilityDropdown"
            label="$i18n{bookmarksBar}"
            pref-key="bookmark_bar.visibility_state"
            .menuOptions="${this.bookmarksBarOptions_}"
            @settings-control-change="${this.onBookmarksBarVisibilitySettingsControlChange_}">
        </settings-dropdown-menu>
      </div>
    ` : ''}


    <settings-toggle-button class="hr" id="showSavedTabGroups"
        pref-key="bookmark_bar.show_tab_groups"
        label="$i18n{showTabGroupsInBookmarksBar}"
        ?hidden="${this.showOrganizerPanelEnabled_}">
    </settings-toggle-button>

    </details>
    <details id="noventraToolsPanelPosition" class="space-details settings-options" ?open="${this.noventraSearchMode}">
      <summary>Tools panel position</summary>
      <p>Choose where bookmarks and other tool panels open. Arrange your tab sidebar in Layout.</p>
    <div class="cr-row">$i18n{sidePanelPosition}</div>
    <div class="list-frame indented-rows">
      <div class="cr-row continuation">
        <div class="flex cr-padded-text" aria-hidden="true">
          Default position
        </div>
        <settings-dropdown-menu id="sidePanelPosition"
            label="${this.i18n('sidePanelAlignmentA11yLabel',
                'Default position',
                '$i18n{sidePanelPosition}')}"
            pref-key="side_panel.is_right_aligned"
            .menuOptions="${this.sidePanelAlignmentOptions_}">
        </settings-dropdown-menu>
      </div>
      <div class="cr-row secondary">Overrides for individual panels</div>
      ${this.configurableSidePanels_.map(item => html`
        <div class="cr-row continuation">
          <div class="flex cr-padded-text" aria-hidden="true">${item.label}</div>
          <settings-dropdown-menu
              label="${this.i18n('sidePanelAlignmentA11yLabel', item.label,
                  '$i18n{sidePanelPosition}')}"
              .menuOptions="${this.sidePanelAlignmentOptions_}"
              data-entry-id="${item.id}"
              pref-key="side_panel.is_right_aligned"
              no-set-pref
              value="${this.getOverrideValue_(item.id)}"
              @settings-control-change="${this.onOverrideAlignmentSettingsControlChange_}">
          </settings-dropdown-menu>
        </div>
      `)}

    </div>
    </details>
    <details id="noventraNativeWindow" class="space-details settings-options"
        ?open="${this.noventraSearchMode}">
      <summary>Window style</summary>
    <settings-toggle-button
        pref-key="helium.browser.rounded_frame"
        label="$i18n{roundedFrame}">
    </settings-toggle-button>
    <if expr="is_macosx">
      ${this.showGlassEffectEnabled_ ? html`
        <div class="cr-row hr">
          <div class="flex cr-padded-text" aria-hidden="true">
            $i18n{glassEffect}
          </div>
          <settings-dropdown-menu id="glassEffect"
              label="$i18n{glassEffect}"
              pref-key="glass_frame.enabled"
              .menuOptions="${this.glassEffectOptions_}"
              @settings-control-change="${this.onGlassFrameSettingsControlChange_}">
          </settings-dropdown-menu>
        </div>
      ` : ''}
    </if>
<if expr="is_linux">
    <div class="hr" ?hidden="${!this.pageVisibility_.bookmarksBar}"></div>
    <settings-toggle-button
        ?hidden="${!this.showCustomChromeFrame_}"
        pref-key="browser.custom_chrome_frame"
        label="$i18n{showWindowDecorations}"
        inverted>
    </settings-toggle-button>
</if>
    </details>
    </section>
    <section id="customizeSpaces" class="customize-section" tabindex="-1"
        aria-labelledby="spacesHeading" ?hidden="${this.isNoventraSectionHidden_('customizeSpaces')}">
    <div id="noventraSpaces" aria-busy="${this.noventraSpacesPending_}">
      <div class="customize-heading">
        <h2 id="spacesHeading">Spaces</h2>
        <p>Keep tabs and website accounts separate for work, personal browsing and more.</p>
      <details id="noventraAboutSpaces" class="settings-help"
          ?open="${this.noventraSearchMode}">
        <summary>About spaces and themes</summary>
        <p>${this.noventraSpaces_[0]?.profileIsolated
            ? 'Each regular space has its own browser profile, including website logins and browsing data.'
            : 'Each space has its own cookies, website logins and site storage.'} Names save when you finish editing; colors, icons and themes save immediately.</p>
        <p>Drag the sidebar buttons or use the arrows below to reorder spaces.</p>
        <p id="noventraSpaceThemeHint">Use browser theme follows your main appearance settings. Another theme applies to the browser controls while that space is active; websites keep their own appearance.</p>
      </details>
      </div>
      <div id="noventraSpacesVisibility" class="cr-row noventra-space-row"
          data-state="${this.spaceSwitcherState_()}"
          aria-busy="${this.noventraShowSpacesPending_}">
        <div class="flex noventra-tool-field">
          <div>Space switcher</div>
          <div id="noventraSpacesVisibilityHint" class="secondary">
            ${this.spaceSwitcherDescription_()}
          </div>
        </div>
        <cr-button id="noventraShowSpaces"
            ?hidden="${this.spaceSwitcherState_() !== 'hidden'}"
            ?disabled="${this.noventraShowSpacesPending_}"
            aria-describedby="noventraSpacesVisibilityHint"
            @click="${this.onShowSpacesInLayoutClick_}">Show in layout</cr-button>
      </div>
      <div id="noventraSpacesVisibilityError" role="alert" class="cr-row"
          ?hidden="${!this.noventraSpacesVisibilityError_}">
        ${this.noventraSpacesVisibilityError_}
      </div>
      <div class="cr-row noventra-space-row">
        <div class="flex noventra-tool-field">
          <div>Space shortcuts</div>
          <div class="secondary">Choose keys to switch spaces and cycle between them.</div>
        </div>
        <a id="noventraSpaceShortcuts" href="chrome://settings/system/shortcuts">Edit shortcuts</a>
      </div>
      ${repeat(this.activeSpaces_(), space => space.id, (space, index) => html`
        <div class="cr-row noventra-space-row" data-space-row="${space.id}">
          <span class="noventra-space-badge" data-color="${space.color}" aria-hidden="true">
            ${space.icon === 'default' ? html`${space.name.slice(0, 1)}` : html`<svg viewBox="0 0 24 24"><path d="${this.spaceIconPath_(space.icon)}"></path></svg>`}
          </span>
          <input class="noventra-space-input flex" aria-label="Space name" data-space="${space.id}"
              .value="${this.noventraSpaceNames_[space.id] ?? space.name}"
              ?disabled="${this.noventraSpacesPending_}"
              aria-invalid="${this.noventraSpaceErrorFor_ === `rename:${space.id}`}"
              aria-describedby="noventraSpaceError"
              @input="${this.onRenameSpaceInput_}" @change="${this.onRenameSpaceChange_}">
          <div class="noventra-space-appearance">
            <label>Color
              <select class="md-select" data-space="${space.id}" data-appearance="color"
                  aria-label="Color for ${space.name}" .value="${space.color}"
                  ?disabled="${this.noventraSpacesPending_ || !space.appearanceEditable}"
                  @change="${this.onSpaceAppearanceChange_}">
                ${this.noventraSpaceColors_.map(color => html`<option value="${color}" ?selected="${space.color === color}">${this.spaceChoiceLabel_(color)}</option>`)}
              </select>
            </label>
            <label>Icon
              <select class="md-select" data-space="${space.id}" data-appearance="icon"
                  aria-label="Icon for ${space.name}" .value="${space.icon}"
                  ?disabled="${this.noventraSpacesPending_ || !space.appearanceEditable}"
                  @change="${this.onSpaceAppearanceChange_}">
                ${this.noventraSpaceIcons_.map(icon => html`<option value="${icon}" ?selected="${space.icon === icon}">${this.spaceChoiceLabel_(icon)}</option>`)}
              </select>
            </label>
<if expr="not is_chromeos">
            <label class="noventra-space-theme">Theme
              <select class="md-select" data-space="${space.id}" data-appearance="theme"
                  aria-label="Theme for ${space.name}" .value="${space.theme}"
                  aria-describedby="noventraSpaceThemeHint"
                  ?disabled="${this.noventraSpacesPending_ || !space.themeEditable}"
                  @change="${this.onSpaceAppearanceChange_}">
                <option value="browser" ?selected="${space.theme === 'browser'}">Use browser theme</option>
                ${this.noventraAppearancePresets_.map(preset => html`<option value="${preset.id}" ?selected="${space.theme === preset.id}">${preset.name}</option>`)}
              </select>
            </label>
</if>
          </div>
          <div class="noventra-space-actions">
            <cr-button data-space="${space.id}" data-action="move-before"
                aria-label="Move ${space.name} earlier" title="Move earlier"
                ?disabled="${this.noventraSpacesPending_ || index === 0}"
                @click="${this.onMoveSpaceClick_}">←</cr-button>
            <cr-button data-space="${space.id}" data-action="move-after"
                aria-label="Move ${space.name} later" title="Move later"
                ?disabled="${this.noventraSpacesPending_ || index === this.activeSpaces_().length - 1}"
                @click="${this.onMoveSpaceClick_}">→</cr-button>
            <cr-button data-space="${space.id}" ?disabled="${this.noventraSpacesPending_}"
                @click="${this.onSwitchSpaceClick_}">Switch</cr-button>
            <cr-button data-space="${space.id}" data-action="archive"
                aria-label="Archive ${space.name}"
                title="${space.id ? 'Hide from the sidebar and keep its tabs and logins' : 'Personal always stays available'}"
                ?disabled="${this.noventraSpacesPending_ || !space.id}"
                @click="${this.onArchiveSpaceClick_}">Archive</cr-button>
          </div>
        </div>`)}
      <div class="cr-row noventra-space-row">
        <input id="noventraSpaceName" class="noventra-space-input flex"
            aria-label="New space name" placeholder="Name a space, e.g. Work"
            .value="${this.noventraNewSpaceName_}" ?disabled="${this.noventraSpacesPending_}"
            aria-invalid="${this.noventraSpaceErrorFor_ === 'create'}"
            aria-describedby="noventraSpaceError" @input="${this.onNewSpaceInput_}"
            @keydown="${this.onNewSpaceKeydown_}">
        <cr-button class="action-button" id="noventraCreateSpace"
            ?disabled="${this.noventraSpacesPending_}" @click="${this.onCreateSpaceClick_}">Create space</cr-button>
      </div>
      <div class="cr-row secondary">Archiving hides a space and keeps its open tabs and logins.</div>
      <section id="noventraArchivedSpaces" aria-labelledby="noventraArchivedHeading"
          ?hidden="${!this.archivedSpaces_().length}">
        <div class="cr-row"><h3 id="noventraArchivedHeading">Archived spaces</h3></div>
        ${repeat(this.archivedSpaces_(), space => space.id, space => html`
          <div class="cr-row noventra-space-row" data-archived-space="${space.id}">
            <span class="noventra-archived-name flex">${space.name}</span>
            <cr-button data-space="${space.id}" data-action="restore"
                aria-label="Restore ${space.name}" ?disabled="${this.noventraSpacesPending_}"
                @click="${this.onRestoreSpaceClick_}">Restore</cr-button>
          </div>`)}
      </section>
      <div id="noventraSpaceStatus" role="status" aria-live="polite"
          class="cr-row secondary" ?hidden="${!this.noventraSpaceStatus_}">${this.noventraSpaceStatus_}</div>
      <details id="noventraWorkspaceRoutes" class="space-details" ?open="${this.noventraSearchMode}">
        <summary>Open domains in a space</summary>
        <p>Open matching links from other apps, bookmarks and browser-created tabs in the chosen space. Existing tabs and page links keep their current space. Match the exact hostname; subdomains need their own rule.</p>
        ${repeat(this.noventraRoutes_, route => route.host, route => html`
          <div class="noventra-workspace-tool-row" data-route-host="${route.host}">
            <span class="flex noventra-rule-name">${route.host} → ${this.workspaceRouteLabel_(route.space_id)}</span>
            <cr-button data-host="${route.host}" aria-label="Remove rule for ${route.host}"
                ?disabled="${this.noventraToolsPending_ || !this.noventraRoutesManageable_}"
                @click="${this.onRemoveWorkspaceRouteClick_}">Remove</cr-button>
          </div>`)}
        <div class="noventra-workspace-tool-row">
          <label class="noventra-tool-field flex">Hostname
            <input id="noventraRouteHost" class="noventra-space-input" placeholder="e.g. github.com"
                .value="${this.noventraRouteHost_}" @input="${this.onWorkspaceRouteHostInput_}"
                @keydown="${this.onWorkspaceRouteKeydown_}"
                ?disabled="${this.noventraToolsPending_ || !this.noventraRoutesManageable_}">
          </label>
          <label class="noventra-tool-field flex">Space
            <select id="noventraRouteSpace" class="md-select" .value="${this.noventraRouteSpace_}"
                @change="${this.onWorkspaceRouteSpaceChange_}"
                ?disabled="${this.noventraToolsPending_ || !this.noventraRoutesManageable_}">
              ${repeat(this.activeSpaces_(), space => space.id, space => html`<option value="${space.id}">${space.name}</option>`)}
            </select>
          </label>
          <cr-button id="noventraSaveRoute" @click="${this.onSaveWorkspaceRouteClick_}"
              ?disabled="${this.noventraToolsPending_ || !this.noventraRoutesManageable_ || !this.noventraRouteHost_.trim()}">Save rule</cr-button>
        </div>
        <p class="secondary">Saving the same hostname replaces its rule. Rules for archived spaces pause until you restore them. Rules do not apply in private windows.</p>
      </details>
      <div id="noventraWorkspaceToolsStatus" role="status" class="cr-row secondary"
          ?hidden="${!this.noventraToolsStatus_}">${this.noventraToolsStatus_}</div>
      <div id="noventraWorkspaceToolsError" role="alert" class="cr-row"
          ?hidden="${!this.noventraToolsError_}">${this.noventraToolsError_}</div>
      <details class="space-details" ?open="${this.noventraSearchMode}"><summary>What is shared between spaces?</summary>
        <p>${this.noventraSpaces_[0]?.profileIsolated
            ? 'History, saved passwords, bookmarks, download history, extensions, settings and notification permissions belong to each regular space. Incognito is temporary. Its bookmarks and history suggestions come from the space that opened it.'
            : 'History, saved passwords, bookmarks, extensions, settings and notification permissions are shared. Web notifications use the space that created them.'}</p>
      </details>
    </div>
    <div id="noventraSpaceError" role="alert" class="cr-row"
        ?hidden="${!this.noventraError_}">${this.noventraError_}</div>
    </section>
    <section id="customizeBehavior" class="customize-section" tabindex="-1"
        aria-labelledby="behaviorHeading" ?hidden="${this.isNoventraSectionHidden_('customizeBehavior')}">
      <div class="customize-heading"><h2 id="behaviorHeading">Tabs & browsing</h2>
        <p>Fine-tune everyday browsing. Changes save immediately.</p></div>
    <h3 class="settings-group-heading">Tabs</h3>
    <settings-toggle-button id="newTabNextToActive"
        pref-key="helium.browser.new_tab_next_to_active"
        label="$i18n{openNewTabNextToActive}">
    </settings-toggle-button>
    <settings-toggle-button class="hr" id="tabCyclingMRU"
        pref-key="helium.browser.mru_tab_cycling"
        label="Cycle tabs in this space by most recently used"
        sub-label="Use Ctrl+Tab to return to recent tabs in the current space.">
    </settings-toggle-button>
    <settings-toggle-button class="hr" id="splitViewDragAndDrop"
        pref-key="browser.split_view_drag_and_drop_enabled"
        label="Drag tabs into the page to split the view"
        sub-label="Drag from the sidebar or top tab bar into the center or an edge of the page.">
    </settings-toggle-button>
    <div class="cr-row">
      <a id="noventraKeyboardShortcuts" href="chrome://settings/system/shortcuts">
        Keyboard shortcuts
      </a>
    </div>
    <h3 class="settings-group-heading">Tab previews</h3>
    ${!this.showHoverCardImagesOption_ ? html`
      <settings-toggle-button id="hoverCardMemoryUsageToggle"
          ?hidden="${!this.pageVisibility_.hoverCard}"
          pref-key="browser.hovercard.memory_usage_enabled"
          label="$i18n{showHoverCardMemoryUsageStandalone}"
            sub-label="Shows an estimate when you hover over a tab. Open Task Manager from More tools for tab and process details.">
      </settings-toggle-button>
    ` : ''}
    ${this.showHoverCardImagesOption_ ? html`
      <div>
        <settings-toggle-button id="hoverCardImagesToggle"
            ?hidden="${!this.pageVisibility_.hoverCard}"
            @settings-boolean-control-change="${this.onHoverCardImagesSettingsBooleanControlChange_}"
            pref-key="browser.hovercard.image_previews_enabled"
            label="$i18n{showHoverCardImages}">
        </settings-toggle-button>
        <settings-toggle-button id="hoverCardMemoryUsageToggle" class="hr"
            ?hidden="${!this.pageVisibility_.hoverCard}"
            @settings-boolean-control-change="${this.onHoverCardMemoryUsageSettingsBooleanControlChange_}"
            pref-key="browser.hovercard.memory_usage_enabled"
            label="$i18n{showHoverCardMemoryUsage}"
            sub-label="Shows an estimate when you hover over a tab. Open Task Manager from More tools for tab and process details.">
        </settings-toggle-button>
      </div>
    ` : ''}
    <h3 class="settings-group-heading">Links</h3>
    <settings-toggle-button id="noventraLinkPreview"
        pref-key="noventra.browser.link_preview"
        label="Link previews"
        sub-label="Take a quick look while keeping your current tab open.">
    </settings-toggle-button>
    <div id="noventraPreviewOptions"
        ?hidden="${!this.noventraLinkPreviewPref_?.value && !this.noventraSearchMode}">
    <div class="cr-row">
      <div class="flex cr-padded-text" aria-hidden="true">Preview shortcut</div>
      <settings-dropdown-menu id="noventraPreviewTrigger"
          label="Preview shortcut"
          pref-key="noventra.browser.link_preview_trigger"
          .menuOptions="${this.noventraPreviewTriggerOptions_}"
          ?disabled="${!this.noventraLinkPreviewPref_?.value}">
      </settings-dropdown-menu>
    </div>
    <settings-toggle-button id="noventraPinnedLinkPreview"
        pref-key="noventra.browser.link_preview_pinned"
        label="Preview off-site links from pinned tabs"
        sub-label="Keep pinned pages open while checking links to other websites."
        ?disabled="${!this.noventraLinkPreviewPref_?.value}">
    </settings-toggle-button>
    </div>
    <details id="noventraAdvancedBrowsing" class="space-details settings-options"
        ?open="${this.noventraSearchMode}">
      <summary>More browsing options</summary>
      <h3 class="settings-group-heading">Keyboard and mouse</h3>
    <settings-toggle-button class="hr" id="shiftRightClickContextMenu"
        pref-key="helium.browser.shift_right_click_context_menu"
        label="$i18n{shiftRightClickContextMenu}">
    </settings-toggle-button>
    <settings-toggle-button class="hr" id="copyPageUrlShortcut"
        pref-key="helium.settings.a11y.copy_page_url_shortcut"
        label="$i18n{copyPageUrlShortcut}">
    </settings-toggle-button>
    <settings-toggle-button class="hr" id="verticalCollapseShortcut"
        pref-key="helium.settings.behavior.vertical_collapse_shortcut"
        label="$i18n{verticalCollapseShortcut}">
    </settings-toggle-button>
    <settings-toggle-button class="hr" id="autoPinNewTabGroups"
        pref-key="auto_pin_new_tab_groups"
        label="$i18n{autoPinNewTabGroups}">
    </settings-toggle-button>
      <h3 class="settings-group-heading">Windows and page controls</h3>
    <settings-toggle-button class="hr" id="noventraWindowMirroring"
        pref-key="noventra.browser.window_mirroring"
        label="Mirror tabs across windows"
        sub-label="Share tabs between windows on this PC without opening a second copy of each page.">
    </settings-toggle-button>
    <details class="space-details" ?open="${this.noventraSearchMode}"><summary>How window mirroring works</summary>
      <p>Window mirroring is off by default. Turn it on to share your tab list between windows on this PC. Each tab has one live page; opening it in another window moves that page there. This does not sync tabs to another device.</p>
    </details>
    <details id="noventraHomePage" class="settings-options" ?open="${this.noventraSearchMode}"><summary>Home page destination</summary>
      <div id="home-button-options" class="list-frame"
          ?hidden="${!this.pageVisibility_.homeButton}">
        <settings-radio-group pref-key="homepage_is_newtabpage">
          <controlled-radio-button class="list-item" name="true"
              pref-key="homepage_is_newtabpage"
              label="$i18n{homePageNtp}" no-extension-indicator>
          </controlled-radio-button>
          <controlled-radio-button id="custom-input" class="list-item"
              name="false" pref-key="homepage_is_newtabpage"
              no-extension-indicator>
            <!-- TODO(dbeam): this can show double indicators when both
                 homepage and whether to use the NTP as the homepage are
                 managed. -->
            <home-url-input id="customHomePage"
                .canTab="${!this.homepageIsNewTabPagePref_?.value}">
            </home-url-input>
          </controlled-radio-button>
          ${this.homepagePref_?.extensionId ? html`
            <extension-controlled-indicator
                extension-id="${this.homepagePref_.extensionId}"
                ?extension-can-be-disabled="${!!this.homepagePref_?.extensionCanBeDisabled}"
                extension-name="${this.homepagePref_.controlledByName}"
                @disable-extension-click="${this.onDisableExtensionClick_}">
            </extension-controlled-indicator>
          ` : ''}
        </settings-radio-group>
      </div>
    </details>
    ${this.showCtrlTabMru_ ? html`
      <settings-toggle-button class="hr" id="ctrlTabMru"
          pref-key="browser.ctrl_tab_mru"
          label="Cycle recent tabs across browser windows"
          sub-label="Experimental global Ctrl+Tab mode. Takes priority over cycling within the current space.">
      </settings-toggle-button>
    ` : ''}




    </details>
    </section>
    `}
  </div>
</settings-section>
${this.showManagedThemeDialog_ ? html`
  <managed-dialog @close="${this.onManagedDialogClose_}"
      title="$i18n{themeManagedDialogTitle}"
      body="$i18n{themeManagedDialogBody}">
  </managed-dialog>
` : ''}
<!--_html_template_end_-->`;
  // clang-format on
}
