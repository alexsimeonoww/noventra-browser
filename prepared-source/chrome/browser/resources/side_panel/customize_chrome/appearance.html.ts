// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {AppearanceElement} from './appearance.js';

export function getHtml(this: AppearanceElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<customize-chrome-theme-snapshot id="themeSnapshot"
    role="button" tabindex="0"
    @keydown="${this.onThemeSnapshotKeydown_}"
    @edit-theme-click="${this.onEditThemeClick_}"
    hidden>
</customize-chrome-theme-snapshot>
<a href="chrome://settings/appearance" target="_blank">Customize Noventra</a>
<customize-chrome-hover-button id="thirdPartyThemeLinkButton"
    class="link-out-button theme-button"
    ?hidden="${!this.thirdPartyThemeId_}"
    @click="${this.onThirdPartyThemeLinkButtonClick_}"
    label="${this.thirdPartyThemeName_}"
    label-description="$i18n{currentTheme}">
</customize-chrome-hover-button>
<div id="editButtonsContainer" ?hidden="${!this.showEditTheme_ && !this.showClassicChromeButton_}">
  <hr class="sp-hr">
  <cr-button id="editThemeButton"
      ?hidden="${!this.showEditTheme_}"
      @click="${this.onUploadedImageButtonClick_}">
    <div id="editThemeIcon" class="cr-icon edit-theme-icon" slot="prefix-icon"
        ?hidden="${this.wallpaperSearchButtonEnabled_}"></div>
    $i18n{changeWallpaper}
  </cr-button>
  <cr-button id="setClassicChromeButton"
      ?hidden="${!this.showClassicChromeButton_}"
      @click="${this.onSetClassicChromeClick_}">
    <div id="setClassicChromeIcon" class="cr-icon" slot="prefix-icon"></div>
    $i18n{resetToDefaultButtonLabel}
  </cr-button>
</div>
${this.showManagedDialog_ ? html`
  <managed-dialog @close="${this.onManagedDialogClose_}"
      title="$i18n{managedColorsTitle}"
      body="$i18n{managedColorsBody}">
  </managed-dialog>
` : ''}
<!--_html_template_end_-->`;
  // clang-format on
}
