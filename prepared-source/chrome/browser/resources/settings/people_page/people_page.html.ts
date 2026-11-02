// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import {ChromeSigninAccessPoint} from '/shared/settings/people_page/sync_browser_proxy.js';

import type {SettingsPeoplePageElement} from './people_page.js';

export function getHtml(this: SettingsPeoplePageElement) {
  return html`<!--_html_template_start_-->
<settings-section page-title="$i18n{peoplePageTitle}">
      <div>

<if expr="not is_chromeos">
        ${this.shouldLinkToProfileRow_() ? html`
          <div id="profile-row" class="cr-row first two-line"
              ?actionable="${this.isProfileActionable_}"
              @click="${this.onProfileClick_}">
            ${this.syncStatus ? html`
              <div id="profile-icon"
                  style="background-image: ${this.getIconImageSet_(
                      this.profileIconUrl_)}">
              </div>
              <div class="flex cr-row-gap cr-padded-text text-elide">
                <span id="profile-name">${this.profileName_}</span>
              </div>
              <cr-icon-button class="subpage-arrow"
                  aria-label="$i18n{editPerson}"
                  aria-describedby="profile-name"
                  aria-roledescription="$i18n{subpageArrowRoleDescription}">
              </cr-icon-button>
            ` : ''}
          </div>
        ` : ''}
</if>

<if expr="is_chromeos">
        ${!this.shouldLinkToAccountSettingsPage_() ? html`
          <div id="profile-row" class="cr-row first two-line"
              ?actionable="${this.isProfileActionable_}"
              @click="${this.onProfileClick_}">
            ${this.syncStatus ? html`
              <div id="profile-icon"
                  style="background-image: ${this.getIconImageSet_(
                      this.profileIconUrl_)}">
              </div>
              <div class="flex cr-row-gap cr-padded-text text-elide">
                <span id="profile-name">${this.profileName_}</span>
                <div class="secondary" ?hidden="${!this.isSyncing_()}">
                  ${this.syncStatus.signedInUsername}
                </div>
              </div>
              <cr-icon-button class="icon-external"
                  id="profile-subpage-arrow"
                  ?hidden="${!this.isProfileActionable_}"
                  aria-label="$i18n{accountManagerSubMenuLabel}"
                  aria-describedby="profile-name">
              </cr-icon-button>
            ` : ''}
          </div>
        ` : ''}
</if>

<if expr="not is_chromeos">
        <cr-link-row id="noventraSetup" label="Set up Noventra"
            sub-label="Welcome, bookmarks and passwords"
            @click="${this.onNoventraSetupClick_}">
        </cr-link-row>
        <cr-link-row id="importDataDialogTrigger"
            label="$i18n{importTitle}"
            @click="${this.onImportDataClick_}">
        </cr-link-row>
</if>

      </div>
    </settings-section>

<if expr="not is_chromeos">
    ${this.showSignoutDialog_ ? html`
      <settings-signout-dialog .syncStatus="${this.syncStatus}"
          @close="${this.onDisconnectDialogClose_}">
      </settings-signout-dialog>
    ` : ''}

    ${this.showImportDataDialog_ ? html`
      <settings-import-data-dialog
          @close="${this.onImportDataDialogClose_}">
      </settings-import-data-dialog>
    ` : ''}
</if>
    <cr-toast duration="3000" id="toast">
      <span>$i18n{syncSettingsSavedToast}</span>
    </cr-toast>
<!--_html_template_end_-->`;
}
