// Copyright 2026 The Noventra Authors. All rights reserved.
import {html, nothing} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {TemplateResult} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {NoventraTransferElement} from './noventra_transfer.js';

export function getHtml(this: NoventraTransferElement): TemplateResult {
  return html`
    <div class="heading"><h2>Data and devices</h2>
      <p class="intro">Move your browser to another PC, or keep an encrypted backup. No Noventra account or subscription is needed.</p>
      <p><a href="chrome://setup/" target="_blank" rel="noopener">Import from another browser or add a password manager</a></p>
    </div>
    <fieldset class="choices" ?disabled="${this.busy_ || !this.available_}">
        <legend>Choose what to include</legend><div class="categories">
        ${this.categoryItems_(false).map(item => html`<label>
          <input type="checkbox" data-category="${item.key}" .checked="${item.checked}"
              @change="${this.onCategoryChange_}"><span>${item.label}</span></label>`)}</div>
      </fieldset>
    <p class="note">Passwords are optional. Existing passwords on the receiving PC are kept. Website logins, cookies, passkeys, password notes, extension vaults, browsing history and unsaved forms are not included. Tabs restore their URLs. Extensions need to be reinstalled and signed in.</p>
    <div class="cards">
      <fieldset class="card" aria-labelledby="fileHeading" ?disabled="${this.busy_ || this.directActive_()}"><h3 id="fileHeading">Encrypted file</h3>
        <p>Save a file on this PC and open it on another. Keep the password separately. The old PC can be offline when you restore.</p>
        <label class="field">Transfer password
          <input id="transferPassword" type="password" autocomplete="new-password" spellcheck="false"
              .value="${this.password_}" @input="${this.onTransferPasswordInput_}" ?disabled="${this.busy_}">
        </label>
        <p class="note">Use at least 12 characters. Spaces and capitalization are preserved.</p>
        <label class="field">Repeat password to save a new file
          <input id="repeatTransferPassword" type="password" autocomplete="new-password" spellcheck="false"
              .value="${this.repeatPassword_}" @input="${this.onRepeatTransferPasswordInput_}" ?disabled="${this.busy_}">
        </label>
        <div class="row"><button id="exportTransfer" class="primary" @click="${this.onExportTransferClick_}"
            ?disabled="${this.busy_ || !this.available_ || !this.hasSelectedCategories_() || !this.validPassword_() || this.password_ !== this.repeatPassword_}">Save encrypted file</button></div>
        <div class="row"><button id="chooseTransfer" @click="${this.onChooseTransferClick_}" ?disabled="${this.busy_ || !this.available_}">Choose transfer file</button>
          <input id="transferFile" type="file" accept=".noventra,application/octet-stream" hidden @change="${this.onTransferFileChange_}"></div>
        ${this.fileName_ ? html`<p class="file-name">${this.fileName_}</p>` : nothing}
        <button id="previewTransfer" @click="${this.onPreviewTransferClick_}"
            ?disabled="${this.busy_ || !this.fileData_ || !this.validPassword_()}">Unlock and review</button>
      </fieldset>
      <section class="card" aria-labelledby="directHeading"><h3 id="directHeading">Direct transfer</h3>
        <p>Keep Settings open on both PCs. Exchange a connection code and a reply privately, compare the device codes, then send. Codes expire after 10 minutes.</p>
        <label><input id="transferStun" type="checkbox" .checked="${this.useStun_}"
            @change="${this.onTransferStunChange_}" ?disabled="${this.isPeerGathering_() || this.isPeerConnected_()}">
          <span>Help connect across networks using Cloudflare’s free STUN service</span></label>
        <p class="note">STUN discovers network addresses; it does not receive your browser data. No paid relay or pairing server is used. Some networks block direct connections; use the file option if that happens.</p>
        <div class="row">
          <button id="startDirectSend" @click="${this.onStartDirectSendClick_}" ?disabled="${this.busy_ || this.isPeerGathering_() || this.peerState_?.phase === 'transferring' || !this.available_ || !this.hasSelectedCategories_()}">Send from this PC</button>
          <button id="startDirectReceive" @click="${this.onStartDirectReceiveClick_}"
              ?disabled="${this.busy_ || this.isPeerGathering_() || this.peerState_?.phase === 'transferring' || !this.available_}">Receive on this PC</button>
        </div>
        ${this.peerState_?.code ? html`<label class="field">${this.peerState_?.sender ? 'Connection code' : 'Reply code'}
            <textarea id="outgoingCode" readonly spellcheck="false" .value="${this.peerState_?.code}"></textarea></label>
          <button id="copyDirectCode" @click="${this.onCopyDirectCodeClick_}">Copy code</button>` : nothing}
        ${this.peerRole_ && !this.isPeerConnected_() && !['sent', 'received', 'transferring'].includes(this.peerState_?.phase ?? '') ? html`
          <label class="field">${this.peerRole_ === 'send' ? 'Reply from the receiving PC' : 'Connection code from the sending PC'}
            <textarea id="incomingCode" spellcheck="false" autocomplete="off" .value="${this.incomingCode_}"
                @input="${this.onIncomingDirectCodeInput_}" ?disabled="${this.isPeerGathering_()}"></textarea></label>
          <button id="connectDirect" @click="${this.onConnectDirectClick_}" ?disabled="${this.isPeerGathering_() || !this.incomingCode_.trim() || this.busy_}">${this.peerRole_ === 'send' ? 'Use reply' : 'Create reply'}</button>` : nothing}
        ${this.isPeerConnected_() && this.peerState_ ? html`<p>Check that this device code matches on both PCs:</p>
          <div id="deviceComparison" class="comparison">${this.peerState_?.comparison}</div>
          <div class="row"><button id="confirmDirectDevice" @click="${this.onConfirmDirectClick_}"
              ?disabled="${this.peerState_?.localConfirmed || !this.peerState_?.comparison}">${this.peerState_?.localConfirmed ? 'This device confirmed' : 'The codes match'}</button></div>
          <p>${this.peerState_?.remoteConfirmed ? 'The other PC has confirmed.' : 'Waiting for the other PC to confirm.'}</p>
          ${this.peerState_?.sender ? html`<button id="sendDirectTransfer" class="primary" @click="${this.onSendDirectClick_}"
              ?disabled="${this.busy_ || !this.peerState_?.localConfirmed || !this.peerState_?.remoteConfirmed || !this.hasSelectedCategories_()}">Send selected data</button>` : nothing}` : nothing}
        ${this.peerState_?.message ? html`<p id="directTransferStatus" role="status">${this.peerState_?.message}</p>` : nothing}
        ${this.peerState_?.phase === 'transferring' ? html`<progress class="progress" max="100" .value="${this.peerState_?.progress}" aria-label="Transfer progress"></progress>` : nothing}
        ${this.peerState_ && !['closed', 'error', 'idle'].includes(this.peerState_?.phase) ? html`<button id="closeDirectTransfer" @click="${this.onCloseDirectClick_}" ?disabled="${this.busy_}">Disconnect</button>` : nothing}
      </section>
    </div>
    ${this.review_ ? html`<section id="transferReview" class="review" tabindex="-1" aria-labelledby="transferReviewHeading">
      <h3 id="transferReviewHeading">Review before importing</h3>
      <p>Nothing has been imported yet. Existing bookmarks, tabs, Spaces and passwords will be kept. Appearance and layout change only if selected below.</p>
      ${Object.values(this.review_.excluded).some(Boolean) ? html`<p>Local-only or unsupported items excluded: ${this.review_.excluded.bookmarks} bookmarks, ${this.review_.excluded.tabs} tabs and ${this.review_.excluded.extensions} extensions. These can include local pages and built-in extensions.</p>` : nothing}
      <div class="counts">
        ${[['Bookmarks', this.review_.bookmarks], ['Spaces', this.review_.spaces], ['Tabs', this.review_.tabs],
            ['Pins', this.review_.pins], ['Extensions', this.review_.extensions],
            ['New passwords', this.review_.password_counts?.new_passwords ?? 0]].map(([label, value]) => html`
            <div class="count"><strong>${value}</strong><span>${label}</span></div>`)}
      </div>
      <fieldset class="choices" ?disabled="${this.busy_ || !this.available_}">
        <legend>Choose what to import</legend><div class="categories">
        ${this.categoryItems_(true).map(item => html`<label>
          <input type="checkbox" data-category="${item.key}" data-review .checked="${item.checked}"
              @change="${this.onCategoryChange_}"><span>${item.label}</span></label>`)}</div>
      </fieldset>
      ${this.review_.password_counts?.kept_passwords ? html`<p>${this.review_.password_counts.kept_passwords} existing or repeated passwords will be kept without replacement.</p>` : nothing}
      <div class="row"><button id="applyTransfer" class="primary" @click="${this.onApplyTransferClick_}"
          ?disabled="${this.busy_ || !Object.values(this.selection_).some(Boolean)}">Import selected data</button>
        <button id="cancelTransferReview" @click="${this.onCancelTransferClick_}" ?disabled="${this.busy_}">Cancel</button></div>
      <p class="note">This review expires after 10 minutes. Existing website sessions stay on their original PC.</p>
    </section>` : nothing}
    ${this.busy_ ? html`<p role="status">Preparing your transfer…</p>` : nothing}
    <div id="transferError" class="error" role="alert" ?hidden="${!this.error_}">${this.error_}</div>
    <div id="transferStatus" class="status" role="status" tabindex="-1" ?hidden="${!this.status_}">${this.status_}</div>
    ${this.extensions_.length ? html`<section aria-label="Reinstall extensions"><h3>Reinstall your extensions</h3>
      <p>Review each extension’s permissions in the Chrome Web Store. Password managers will ask you to sign in or unlock again.</p>
      ${this.extensions_.map(extension => html`<div class="extension"><span>${extension.name}</span>
        <a href="${'https://chromewebstore.google.com/detail/' + extension.id}" target="_blank" rel="noopener noreferrer">Open in Web Store</a></div>`)}
    </section>` : nothing}
    <p class="note">Direct transfer is a one-time move. It does not keep devices synchronized after disconnecting. For another transfer, create fresh codes or save a new file.</p>
  `;
}
