// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import type {ComponentsData} from './components.js';

export function getHtml(data: ComponentsData) {
  return html`<!--_html_template_start_-->
<div id="container">
  <div id="top">
    <div class="section-header">
      <span class="section-header-title">$i18n{componentsTitle}</span>
      <span class="section-header-title"
          ?hidden="${data.components.length === 0}">
        (${data.components.length})
      </span>
    </div>
  </div>

  <div class="content">
    <section ?hidden="${!data.widevineAvailable}"
        aria-label="Protected streaming">
      <h2>Protected streaming</h2>
      <p>Some streaming services need Widevine from Google. Enabling downloads
        installs it and keeps it updated.</p>
      <p id="widevine-not-installed" role="status"
          ?hidden="${data.components.some(item =>
              item.widevine && item.version !== '0.0.0.0')}">
        Widevine is not installed. Allow downloads below, wait for an installed
        version to appear, then restart Noventra and reload your streaming page.
        If installation fails, use Check for update and review its status below.
      </p>
      <label>
        <input id="widevine-downloads" type="checkbox"
            .checked="${!!data.widevineDownloadsEnabled}"
            ?disabled="${!data.widevineCanChange || data.widevineDownloadsBusy}">
        Allow Widevine downloads and updates from Google
      </label>
      <p>This sends component and device information to Google.
        Turning this off stops new update checks; downloads already started may finish.
        An installed copy remains available.
        Website playback is controlled in
        <a href="chrome://settings/content/protectedContent">Protected content</a>.</p>
      <p id="widevine-downloads-error" role="alert"
          ?hidden="${!data.widevineDownloadsError}">
        Could not save the download preference. Try again.</p>
      <p id="widevine-restart" role="status"
          ?hidden="${!data.widevineRestartRequired}">
        Widevine is installed. Restart Noventra to use the new version.</p>
    </section>
    <div class="component-name no-components"
        ?hidden="${data.components.length > 0}">
      <div>$i18n{noComponents}</div>
    </div>

    <div ?hidden="${data.components.length === 0}">
      ${data.components.map(item => html`
        <div class="component">
          <div class="component-enabled">
            <div class="component-text">
              <div>
                <span class="component-name" dir="ltr">${item.name}</span>
                <span>
                  <span> - </span>
                  <span>$i18n{componentVersion}</span>
                  <span dir="ltr" id="version-${item.id}">
                    ${item.version}
                  </span>
                </span>
              </div>
            </div>
          </div>
          <div class="component-text">
            <span>$i18n{statusLabel}</span>
            -
            <span id="status-${item.id}">${item.status}</span>
          </div>
          <div class="component-actions">
            <button class="button-check-update" guest-disabled id="${item.id}"
                ?disabled="${item.widevine &&
                    (!data.widevineDownloadsEnabled || !data.widevineCanChange ||
                     data.widevineDownloadsBusy)}">
              $i18n{checkUpdate}
            </button>
            <button class="button-uninstall" guest-disabled id="${item.id}"
                ?hidden="${!data.showUninstallButton}">
              $i18n{uninstall}
            </button>
          </div>
        </div>
      `)}
    </div>
  </div>
</div>
<!--_html_template_end_-->`;
}
