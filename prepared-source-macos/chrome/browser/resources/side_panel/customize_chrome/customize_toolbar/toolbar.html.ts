// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {ToolbarElement} from './toolbar.js';

export function getHtml(this: ToolbarElement) {
  return html`<!--_html_template_start_-->
<div class="sp-card" id="pinningSelectionCard">
  <sp-heading id="heading" class="separator"
      @back-button-click="${this.onBackButtonClick_}"
      back-button-aria-label="$i18n{backButton}"
      back-button-title="$i18n{backButton}">
    <h2 slot="heading">$i18n{toolbarHeader}</h2>
  </sp-heading>
  <div class="customization-content">
    <p id="noventraToolbarDescription">
      Choose toolbar controls and arrange your browser layout in Settings.
    </p>
    <cr-button id="noventraCustomizeLayout"
        aria-describedby="noventraToolbarDescription"
        @click="${this.onCustomizeLayoutClick_}">
      Customize Noventra
    </cr-button>
  </div>
</div>
<!--_html_template_end_-->`;
}
