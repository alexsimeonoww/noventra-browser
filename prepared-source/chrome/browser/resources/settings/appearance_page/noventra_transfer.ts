// Copyright 2026 The Noventra Authors. All rights reserved.
import {CrLitElement, css} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {addWebUiListener, removeWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';
import {NoventraPeerTransfer, transferBase64ToBytes, transferBytesToBase64} from './noventra_peer_transfer.js';
import type {PeerTransferState} from './noventra_peer_transfer.js';
import {hasPendingLayoutChanges} from '../noventra_navigation_guard.js';
import {getHtml} from './noventra_transfer.html.js';

const CATEGORY_LABELS = {
  bookmarks: 'Bookmarks', spaces: 'Spaces', tabs: 'Open tabs and pins',
  settings: 'Appearance and layout', extensions: 'Extension reinstall list',
  passwords: 'Saved browser passwords',
} as const;
type Category = keyof typeof CATEGORY_LABELS;


type Categories = Record<Category, boolean>;
interface Review {
  plan_id: string;
  transfer_id: string;
  created_at: string;
  categories: Categories;
  bookmarks: number;
  bookmark_folders: number;
  spaces: number;
  tabs: number;
  pins: number;
  settings: number;
  extensions: number;
  extension_items: Array<{id: string, name: string}>;
  excluded: {bookmarks: number, tabs: number, extensions: number};
  password_counts?: {passwords: number, new_passwords: number, kept_passwords: number};
}
interface ExportedTransfer {data: string; bytes: number; filename: string; summary: Review;}

export class NoventraTransferElement extends CrLitElement {
  static get is() { return 'noventra-transfer'; }
  static override get styles(): CSSResultGroup {
    return css`
      :host {display:block;color:var(--cr-primary-text-color);--line:var(--cr-separator-color)}
      * {box-sizing:border-box} [hidden] {display:none!important}
      h2 {font-size:18px;font-weight:500;margin:0 0 8px} h3 {font-size:14px;font-weight:500;margin:0 0 8px}
      p {font-size:12px;line-height:1.65;color:var(--cr-secondary-text-color);margin:8px 0 14px}
      .cards {display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin-block:18px}
      .card,.review,.choices {border:1px solid var(--line);border-radius:12px;padding:18px;min-width:0}
      .choices {margin-block:18px;padding:14px 18px} .choices legend {padding-inline:4px;font-weight:500;font-size:13px}
      .categories {display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
      label {display:flex;align-items:start;gap:9px;font-size:12px;line-height:1.5;min-width:0}
      input[type=checkbox] {accent-color:var(--cr-focus-outline-color);flex:none;margin:3px 0}
      button,input,textarea {font:inherit} button {min-height:36px;border-radius:8px;border:1px solid var(--line);padding:7px 12px;color:inherit;background:transparent;cursor:pointer;font-size:12px}
      button:hover:enabled {background:var(--cr-hover-background-color)} button:disabled {opacity:.5;cursor:default}
      button.primary {color:var(--cr-button-text-color);background:var(--cr-button-background-color);border-color:var(--cr-button-background-color)}
      button:focus-visible,input:focus-visible,textarea:focus-visible,a:focus-visible {outline:2px solid var(--cr-focus-outline-color);outline-offset:3px}
      .row {display:flex;align-items:center;flex-wrap:wrap;gap:8px;margin-block:12px}
      .field {display:flex;flex-direction:column;gap:6px;flex:1;min-width:0;font-size:12px;margin-block:10px}
      input[type=password],textarea {color:inherit;background:var(--cr-card-background-color);border:1px solid var(--line);border-radius:8px;padding:9px 10px;width:100%;min-width:0}
      textarea {font-family:monospace;font-size:11px;resize:vertical;min-height:76px;overflow-wrap:anywhere}
      .file-name {overflow-wrap:anywhere;font-size:12px;color:var(--cr-secondary-text-color)}
      .status,.error {font-size:12px;line-height:1.6;margin-block:12px;overflow-wrap:anywhere}
      .error {color:var(--cr-primary-text-color);border-inline-start:3px solid var(--cr-focus-outline-color);padding-inline-start:10px}
      .counts {display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;margin-block:16px}
      .count strong {display:block;font-size:20px;font-weight:500}.count span {font-size:11px;color:var(--cr-secondary-text-color)}
      .comparison {font:600 19px monospace;letter-spacing:1px;padding:12px;background:var(--cr-hover-background-color);border-radius:8px;overflow-wrap:anywhere}
      .progress {width:100%;accent-color:var(--cr-focus-outline-color)}
      a {color:var(--cr-link-color);text-underline-offset:3px} .extension {display:flex;gap:12px;justify-content:space-between;align-items:center;padding-block:8px;border-bottom:1px solid var(--line);font-size:12px}.extension span {overflow-wrap:anywhere;min-width:0}
      .extension a {flex:none} .note {font-size:11px}.intro {max-width:720px}.heading {margin-block-end:20px}
      @media(max-width:640px) {.cards {grid-template-columns:1fr}.categories {grid-template-columns:1fr}.counts {grid-template-columns:repeat(2,minmax(0,1fr))}}
      @media(prefers-reduced-motion:reduce) {* {scroll-behavior:auto}}
    `;
  }

  override render() {
    return getHtml.call(this);
  }

  static override get properties() {
    return {
      busy_: {type: Boolean}, available_: {type: Boolean}, error_: {type: String},
      status_: {type: String}, categories_: {type: Object}, password_: {type: String},
      repeatPassword_: {type: String}, fileName_: {type: String}, review_: {type: Object},
      selection_: {type: Object}, extensions_: {type: Array}, peerState_: {type: Object},
      peerRole_: {type: String}, incomingCode_: {type: String}, useStun_: {type: Boolean}, fileData_: {type: String},
    };
  }

  protected accessor busy_ = false;
  protected accessor available_ = false;
  protected accessor error_ = '';
  protected accessor status_ = '';
  protected accessor categories_: Categories = {bookmarks: true, spaces: true, tabs: true, settings: true, extensions: true, passwords: false};
  protected accessor password_ = '';
  protected accessor repeatPassword_ = '';
  protected accessor fileName_ = '';
  protected accessor review_: Review|null = null;
  protected accessor selection_: Categories = {...this.categories_};
  protected accessor extensions_: Array<{id: string, name: string}> = [];
  protected accessor peerState_: PeerTransferState|null = null;
  protected accessor peerRole_: 'send'|'receive'|'' = '';
  protected accessor incomingCode_ = '';
  protected accessor useStun_ = true;
  protected accessor fileData_ = '';
  private maxBytes_ = 32 * 1024 * 1024 + 64;
  private generation_ = 0;
  protected peer_: NoventraPeerTransfer|null = null;
  private peerAttempt_ = 0;
  private peerGrant_ = '';
  private peerStopped_: WebUiListener|null = null;
  private readonly leaving_ = (event: BeforeUnloadEvent) => {
    if (this.busy_ || this.review_ || ['connecting', 'connected', 'transferring'].includes(this.peerState_?.phase ?? '')) {
      event.preventDefault();
      event.returnValue = '';
    }
  };

  override connectedCallback() {
    super.connectedCallback();
    window.addEventListener('beforeunload', this.leaving_);
    this.peerStopped_ = addWebUiListener('noventra-transfer-peer-stopped', (id: string) => {
      if (id === this.peerGrant_)
        this.peer_?.close('The connection expired. Create fresh codes or use a file.');
    });
    void sendWithPromise<{available: boolean, max_bytes: number}>('noventraTransfer', 'status', {})
        .then(result => { if (this.isConnected) { this.available_ = result.available; this.maxBytes_ = result.max_bytes; } })
        .catch(() => { this.error_ = 'Transfers are available in a regular Noventra profile.'; });
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    ++this.generation_;
    window.removeEventListener('beforeunload', this.leaving_);
    if (this.peerStopped_) removeWebUiListener(this.peerStopped_);
    this.peerStopped_ = null;
    this.resetPeer_();
    this.password_ = this.repeatPassword_ = this.fileData_ = this.incomingCode_ = '';
    this.review_ = null;
    void sendWithPromise('noventraTransfer', 'cancel', {}).catch(() => {});
  }

  protected validPassword_(): boolean {
    const length = new TextEncoder().encode(this.password_).length;
    return Array.from(this.password_).length >= 12 && length <= 1024;
  }

  protected directActive_(): boolean {
    return !!this.peerState_ &&
        !['idle', 'closed', 'error', 'sent', 'received'].includes(this.peerState_.phase);
  }

  protected toggle_(key: Category, checked: boolean, review = false) {
    const next = {...(review ? this.selection_ : this.categories_), [key]: checked};
    if (key === 'tabs' && checked) next.spaces = true;
    if (key === 'spaces' && !checked) next.tabs = false;
    if (review) this.selection_ = next;
    else this.categories_ = next;
  }

  protected categoryItems_(review = false) {
    const values = review ? this.selection_ : this.categories_;
    return (Object.keys(CATEGORY_LABELS) as Category[])
        .filter(key => !review || this.review_?.categories[key])
        .map(key => ({key, label: CATEGORY_LABELS[key], checked: values[key]}));
  }

  protected hasSelectedCategories_(review = false): boolean {
    return Object.values(review ? this.selection_ : this.categories_).some(Boolean);
  }

  protected isPeerConnected_(): boolean { return this.peerState_?.phase === 'connected'; }
  protected isPeerGathering_(): boolean { return this.peerState_?.phase === 'gathering'; }

  protected onCategoryChange_(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    const key = input.dataset['category'] as Category;
    if (Object.hasOwn(CATEGORY_LABELS, key))
      this.toggle_(key, input.checked, input.hasAttribute('data-review'));
  }

  protected onTransferPasswordInput_(event: Event) {
    this.password_ = (event.currentTarget as HTMLInputElement).value;
  }
  protected onRepeatTransferPasswordInput_(event: Event) {
    this.repeatPassword_ = (event.currentTarget as HTMLInputElement).value;
  }
  protected onTransferStunChange_(event: Event) {
    this.useStun_ = (event.currentTarget as HTMLInputElement).checked;
  }
  protected onIncomingDirectCodeInput_(event: Event) {
    this.incomingCode_ = (event.currentTarget as HTMLTextAreaElement).value;
  }
  protected onPreviewTransferClick_() { return this.preview_(); }
  protected onCancelTransferClick_() { return this.cancelReview_(); }
  protected onConfirmDirectClick_() { this.peer_?.confirm(); }
  protected onCloseDirectClick_() { this.peer_?.close(); }
  protected onStartDirectReceiveClick_() {
    this.resetPeer_(); this.peerRole_ = 'receive'; this.incomingCode_ = '';
  }

  private async work_<T>(action: string, payload: object): Promise<T|null> {
    if (this.busy_ || !this.available_) return null;
    const generation = ++this.generation_;
    this.busy_ = true;
    this.error_ = '';
    this.status_ = '';
    try {
      const result = await sendWithPromise<T>('noventraTransfer', action, payload);
      return this.isConnected && this.generation_ === generation ? result : null;
    } catch (error) {
      if (this.isConnected && this.generation_ === generation)
        this.error_ = typeof error === 'string' ? error : 'The transfer could not be completed. Try again.';
      return null;
    } finally {
      if (this.generation_ === generation) this.busy_ = false;
    }
  }

  protected async onExportTransferClick_() {
    if (this.directActive_() || !this.validPassword_() || this.password_ !== this.repeatPassword_) return;
    this.review_ = null;
    const result = await this.work_<ExportedTransfer>('export', {categories: this.categories_, password: this.password_});
    this.password_ = this.repeatPassword_ = '';
    if (!result) return;
    const bytes = transferBase64ToBytes(result.data);
    const url = URL.createObjectURL(new Blob([new Uint8Array(bytes)], {type: 'application/octet-stream'}));
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = result.filename;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    this.status_ = 'Encrypted transfer file prepared. Keep its password separately; you will need it on the other PC.';
    const skipped = Object.values(result.summary.excluded).reduce((sum, value) => sum + value, 0);
    if (skipped) this.status_ += ` ${skipped} local-only or unsupported items were excluded.`;
  }

  protected onChooseTransferClick_() {
    if (this.busy_ || this.directActive_()) return;
    const input = this.shadowRoot.querySelector<HTMLInputElement>('#transferFile')!;
    input.value = '';
    input.click();
  }

  protected async onTransferFileChange_(event: Event) {
    const file = (event.target as HTMLInputElement).files?.[0];
    if (!file || this.busy_ || this.directActive_()) return;
    await this.cancelReview_();
    this.error_ = '';
    this.fileName_ = file.name;
    this.fileData_ = '';
    if (file.size < 64 || file.size > this.maxBytes_) {
      this.error_ = 'Choose a Noventra transfer file smaller than 33 MB.'; return;
    }
    const generation = ++this.generation_;
    this.busy_ = true;
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (this.isConnected && generation === this.generation_)
        this.fileData_ = transferBytesToBase64(bytes);
    } catch { this.error_ = 'The file could not be read. Choose it again.'; }
    finally { if (generation === this.generation_) this.busy_ = false; }
  }

  protected async preview_(data = this.fileData_, password = this.password_) {
    if (this.busy_) return;
    this.review_ = null;
    const result = await this.work_<Review>('preview', {data, password});
    this.password_ = this.repeatPassword_ = this.fileData_ = '';
    if (!result) return;
    this.review_ = result;
    this.selection_ = {...result.categories};
    await this.updateComplete;
    this.shadowRoot.querySelector<HTMLElement>('#transferReview')?.focus();
  }

  protected async cancelReview_() {
    if (this.busy_) return;
    this.review_ = null;
    this.status_ = '';
    await sendWithPromise('noventraTransfer', 'cancel', {}).catch(() => {});
  }

  protected async onApplyTransferClick_() {
    const review = this.review_;
    if (!review) return;
    if (this.selection_.settings && hasPendingLayoutChanges()) {
      this.error_ = 'Wait for layout saving to finish, or resolve its save error first, or leave Appearance and layout unchecked.'; return;
    }
    const result = await this.work_<Record<string, unknown>>('apply', {plan_id: review.plan_id, categories: this.selection_});
    if (!result) return;
    this.extensions_ = this.selection_.extensions ? review.extension_items : [];
    this.review_ = null;
    this.status_ = result['partial'] ?
        'Some tabs could not be restored. Completed categories were kept. Choose the same transfer again to retry the remaining data.' : result['password_error'] ?
        'The selected browser data was imported, but passwords did not finish. Existing passwords were kept. Choose the transfer again to retry passwords.' :
        'Import finished. Existing data was kept. Restored tabs and Spaces require a fresh website sign-in.';
    this.fire('noventra-transfer-applied');
    await this.updateComplete;
    this.shadowRoot.querySelector<HTMLElement>('#transferStatus')?.focus();
  }

  private stopPeer_(grant = this.peerGrant_) {
    if (!grant) return;
    if (this.peerGrant_ === grant) this.peerGrant_ = '';
    void sendWithPromise('noventraTransfer', 'peer-stop', {peer_id: grant}).catch(() => {});
  }

  protected resetPeer_() {
    ++this.peerAttempt_;
    const previous = this.peer_;
    this.peer_ = null;
    previous?.close();
    this.stopPeer_();
    this.peerState_ = null;
  }

  private makePeer_(grant: string) {
    this.peer_?.close('');
    const peer = new NoventraPeerTransfer(
        state => {
          if (this.peer_ !== peer) return;
          this.peerState_ = state;
          if (state.message && ['closed', 'error'].includes(state.phase) && this.peerGrant_ === grant) {
            ++this.peerAttempt_;
            this.stopPeer_(grant);
          }
        },
        async (data, password) => {
          if (this.peer_ !== peer || this.busy_)
            throw new Error('The receiving browser is busy. Create new codes to retry.');
          await this.preview_(data, password);
        });
    this.peer_ = peer;
    return peer;
  }

  protected async onStartDirectSendClick_() {
    if (this.busy_) return;
    this.resetPeer_();
    const attempt = this.peerAttempt_;
    await this.cancelReview_();
    if (!this.isConnected || attempt !== this.peerAttempt_) return;
    this.peerRole_ = 'send';
    this.incomingCode_ = '';
    this.error_ = '';
    const grant = crypto.randomUUID();
    this.peerGrant_ = grant;
    try {
      await sendWithPromise('noventraTransfer', 'peer-start', {peer_id: grant});
      if (!this.isConnected || attempt !== this.peerAttempt_) { this.stopPeer_(grant); return; }
      await this.makePeer_(grant).createOffer(this.useStun_);
    } catch {
      this.stopPeer_(grant);
      if (this.isConnected && attempt === this.peerAttempt_)
        this.error_ = 'The connection could not be started. Try again, or use an encrypted file.';
    }
  }

  protected async onConnectDirectClick_() {
    if (this.busy_) return;
    this.error_ = '';
    const code = this.incomingCode_;
    let attempt = this.peerAttempt_;
    let grant = this.peerGrant_;
    try {
      if (this.peerRole_ === 'send') await this.peer_?.acceptAnswer(code);
      else {
        this.resetPeer_();
        attempt = this.peerAttempt_;
        await this.cancelReview_();
        if (!this.isConnected || attempt !== this.peerAttempt_) return;
        this.peerRole_ = 'receive';
        grant = crypto.randomUUID();
        this.peerGrant_ = grant;
        await sendWithPromise('noventraTransfer', 'peer-start', {peer_id: grant});
        if (!this.isConnected || attempt !== this.peerAttempt_) { this.stopPeer_(grant); return; }
        await this.makePeer_(grant).acceptOffer(code, this.useStun_);
      }
      if (!this.isConnected || attempt !== this.peerAttempt_) return;
      this.incomingCode_ = '';
    } catch (error) {
      if (this.peerRole_ === 'receive') this.stopPeer_(grant);
      if (this.isConnected && attempt === this.peerAttempt_)
        this.error_ = error instanceof Error ? error.message : 'That connection code could not be used.';
    }
  }

  protected async onCopyDirectCodeClick_() {
    try { await navigator.clipboard.writeText(this.peerState_?.code ?? ''); }
    catch {
      const field = this.shadowRoot.querySelector<HTMLTextAreaElement>('#outgoingCode');
      field?.focus(); field?.select();
      this.status_ = 'The connection code is selected. Copy it and send it privately to your other PC.';
    }
  }

  protected async onSendDirectClick_() {
    const peer = this.peer_;
    if (!peer?.password) return;
    const result = await this.work_<ExportedTransfer>('export', {categories: this.categories_, password: peer.password});
    if (result && peer === this.peer_) {
      try { await peer.send(result.data); }
      catch { this.error_ = 'The connection is no longer ready. Create new codes or use a file.'; }
    }
  }


}

declare global { interface HTMLElementTagNameMap {'noventra-transfer': NoventraTransferElement;} }
customElements.define(NoventraTransferElement.is, NoventraTransferElement);
