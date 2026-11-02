// Copyright 2026 The Noventra Authors. All rights reserved.

// Signaling is exchanged explicitly by the user. No rendezvous, account,
// analytics or TURN service is contacted. The optional public STUN request only
// discovers network candidates after the user starts a connection.
const MAX_BYTES = 32 * 1024 * 1024 + 64;
const CODE_LIFETIME_MS = 10 * 60 * 1000;
const MAX_CODE_LENGTH = 64 * 1024;
const CHUNK_BYTES = 16 * 1024;

interface ConnectionCode {
  format: 'noventra-direct';
  version: 1;
  role: 'offer'|'answer';
  session: string;
  expires: number;
  sdp: string;
  secret?: string;
}

export interface PeerTransferState {
  phase: 'idle'|'gathering'|'waiting'|'connecting'|'connected'|'transferring'|'received'|'sent'|'closed'|'error';
  message: string;
  code: string;
  comparison: string;
  localConfirmed: boolean;
  remoteConfirmed: boolean;
  sender: boolean;
  progress: number;
}

export function transferBytesToBase64(bytes: Uint8Array): string {
  let text = '';
  for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
    text += String.fromCharCode(...bytes.subarray(offset, offset + CHUNK_BYTES));
  }
  return btoa(text);
}

export function transferBase64ToBytes(text: string): Uint8Array {
  if (text.length > Math.ceil(MAX_BYTES / 3) * 4) throw new Error('Transfer is too large.');
  return Uint8Array.from(atob(text), char => char.charCodeAt(0));
}

export function parseTransferConnectionCode(text: string): ConnectionCode {
  if (text.length > MAX_CODE_LENGTH)
    throw new Error('Paste a complete Noventra connection code.');
  const code = text.trim();
  if (!code.startsWith('NV1-') || code.length > MAX_CODE_LENGTH ||
      !/^[A-Za-z0-9_-]+$/.test(code.slice(4))) {
    throw new Error('Paste a complete Noventra connection code.');
  }
  let value: ConnectionCode;
  try {
    const body = code.slice(4).replace(/-/g, '+').replace(/_/g, '/');
    value = JSON.parse(new TextDecoder('utf-8', {fatal: true}).decode(
        transferBase64ToBytes(body))) as ConnectionCode;
  } catch {
    throw new Error('That connection code is not valid.');
  }
  const now = Date.now();
  if (!value || value.format !== 'noventra-direct' || value.version !== 1 ||
      !['offer', 'answer'].includes(value.role) ||
      typeof value.session !== 'string' ||
      !/^[a-f0-9]{8}-(?:[a-f0-9]{4}-){3}[a-f0-9]{12}$/.test(value.session) ||
      !Number.isSafeInteger(value.expires) || value.expires <= now ||
      value.expires > now + CODE_LIFETIME_MS + 30 * 1000 ||
      typeof value.sdp !== 'string' || value.sdp.length > 32 * 1024 ||
      !value.sdp.startsWith('v=0\r\n') ||
      !/^a=fingerprint:sha-256 (?:[A-Fa-f0-9]{2}:){31}[A-Fa-f0-9]{2}\r?$/m.test(value.sdp) ||
      (value.role === 'offer' && (typeof value.secret !== 'string' ||
          !/^[A-Za-z0-9+/]{43}=$/.test(value.secret)))) {
    throw new Error('That code is invalid or expired. Create a new connection.');
  }
  return value;
}

function encodeCode(value: ConnectionCode): string {
  return 'NV1-' + transferBytesToBase64(new TextEncoder().encode(JSON.stringify(value)))
      .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

async function digest(bytes: Uint8Array): Promise<string> {
  const hash = await crypto.subtle.digest('SHA-256', new Uint8Array(bytes));
  return [...new Uint8Array(hash)].map(value => value.toString(16).padStart(2, '0')).join('');
}

export class NoventraPeerTransfer {
  state: PeerTransferState = {
    phase: 'idle', message: '', code: '', comparison: '', localConfirmed: false,
    remoteConfirmed: false, sender: false, progress: 0,
  };
  private pc: RTCPeerConnection|null = null;
  private channel: RTCDataChannel|null = null;
  private session = '';
  private secret = '';
  private expires = 0;
  private expiryTimer: number|undefined;
  private connectionTimer: number|undefined;
  private manifest: {bytes: number, hash: string}|null = null;
  private chunks: Uint8Array[] = [];
  private receivedBytes = 0;
  private receiveQueue = Promise.resolve();
  private sent = false;
  private endSent = false;
  private generation = 0;
  private comparisonReady: Promise<boolean> = Promise.resolve(false);
  private comparisonResolved: ((ready: boolean) => void)|null = null;
  private readonly changed: (state: PeerTransferState) => void;
  private readonly received: (data: string, password: string) => Promise<void>;

  constructor(changed: (state: PeerTransferState) => void,
              received: (data: string, password: string) => Promise<void>) {
    this.changed = changed;
    this.received = received;
  }

  private update(patch: Partial<PeerTransferState>) {
    this.state = {...this.state, ...patch};
    this.changed({...this.state});
  }

  close(message = 'Connection closed. Create a new code to connect again.') {
    ++this.generation;
    this.comparisonResolved?.(false);
    this.comparisonResolved = null;
    window.clearTimeout(this.expiryTimer);
    window.clearTimeout(this.connectionTimer);
    this.channel?.close();
    this.pc?.close();
    this.channel = null;
    this.pc = null;
    this.secret = '';
    this.session = '';
    this.chunks = [];
    this.manifest = null;
    this.receivedBytes = 0;
    this.receiveQueue = Promise.resolve();
    this.sent = this.endSent = false;
    this.update({phase: 'closed', message, code: '', comparison: '',
      localConfirmed: false, remoteConfirmed: false, progress: 0});
  }

  private fail(message: string) {
    this.close(message);
    this.update({phase: 'error'});
  }

  private createPeer(useStun: boolean) {
    this.comparisonReady = new Promise(resolve => { this.comparisonResolved = resolve; });
    const pc = new RTCPeerConnection({
      iceServers: useStun ? [{urls: 'stun:stun.cloudflare.com:3478'}] : [],
      iceTransportPolicy: 'all',
    });
    this.pc = pc;
    pc.onconnectionstatechange = () => {
      if (this.pc !== pc) return;
      if (pc.connectionState === 'failed') {
        this.fail('A direct connection could not be made. Try the same network, or use an encrypted transfer file.');
      } else if (pc.connectionState === 'disconnected') {
        this.fail('The connection was interrupted. No automatic retry will send data. Create new codes, or use a file.');
      }
    };
    pc.ondatachannel = event => {
      if (this.pc === pc) this.bindChannel(event.channel);
      else event.channel.close();
    };
    this.expiryTimer = window.setTimeout(() => {
      if (this.pc === pc)
        this.fail('This connection expired. Create new codes to connect again.');
    }, Math.max(0, this.expires - Date.now()));
    return pc;
  }

  private async gather(pc: RTCPeerConnection) {
    if (pc !== this.pc || pc.signalingState === 'closed') throw new Error('Connection canceled.');
    if (pc.iceGatheringState === 'complete') return;
    await new Promise<void>((resolve, reject) => {
      const done = () => {
        window.clearTimeout(timer);
        pc.removeEventListener('icegatheringstatechange', check);
        if (pc !== this.pc || pc.signalingState === 'closed') reject(new Error('Connection canceled.'));
        else resolve();
      };
      const check = () => { if (pc.iceGatheringState === 'complete') done(); };
      const timer = window.setTimeout(done, 12 * 1000);
      pc.addEventListener('icegatheringstatechange', check);
    });
  }

  async createOffer(useStun: boolean): Promise<void> {
    this.close('');
    this.sent = false;
    this.session = crypto.randomUUID();
    this.secret = transferBytesToBase64(crypto.getRandomValues(new Uint8Array(32)));
    this.expires = Date.now() + CODE_LIFETIME_MS;
    this.update({phase: 'gathering', sender: true, message: 'Creating a private connection code…'});
    const pc = this.createPeer(useStun);
    this.bindChannel(pc.createDataChannel('noventra-transfer-v1', {ordered: true}));
    try {
      const offer = await pc.createOffer();
      if (this.pc !== pc) return;
      await pc.setLocalDescription(offer);
      if (this.pc !== pc) return;
      await this.gather(pc);
      if (this.pc !== pc) return;
      this.update({phase: 'waiting', message: 'Paste this code on the receiving PC, then paste its reply below.',
        code: encodeCode({format: 'noventra-direct', version: 1, role: 'offer',
          session: this.session, expires: this.expires,
          secret: this.secret, sdp: pc.localDescription!.sdp})});
    } catch {
      if (this.pc === pc) this.fail('The connection code could not be created. Try again, or use a file.');
    }
  }

  async acceptOffer(code: string, useStun: boolean): Promise<void> {
    const offer = parseTransferConnectionCode(code);
    if (offer.role !== 'offer') throw new Error('Paste the code from the sending PC here.');
    this.close('');
    this.sent = false;
    this.session = offer.session;
    this.secret = offer.secret!;
    this.expires = offer.expires;
    this.update({phase: 'gathering', sender: false, message: 'Creating a reply code…'});
    const pc = this.createPeer(useStun);
    try {
      await pc.setRemoteDescription({type: 'offer', sdp: offer.sdp});
      if (this.pc !== pc) return;
      const answer = await pc.createAnswer();
      if (this.pc !== pc) return;
      await pc.setLocalDescription(answer);
      if (this.pc !== pc) return;
      await this.gather(pc);
      if (this.pc !== pc) return;
      const reply = encodeCode({format: 'noventra-direct', version: 1, role: 'answer',
        session: this.session, expires: this.expires, sdp: pc.localDescription!.sdp});
      await this.setComparison(pc);
      if (this.pc !== pc) return;
      this.update({phase: 'connecting', message: 'Paste this reply on the sending PC. Keep both Settings pages open.', code: reply});
      this.waitForConnection();
    } catch {
      if (this.pc === pc) this.fail('That code could not establish a connection. Create new codes, or use a file.');
    }
  }

  async acceptAnswer(code: string): Promise<void> {
    const answer = parseTransferConnectionCode(code);
    const pc = this.pc;
    if (!pc || !this.state.sender || answer.role !== 'answer' ||
        answer.session !== this.session || answer.expires !== this.expires ||
        pc.signalingState !== 'have-local-offer') {
      throw new Error('This reply does not belong to the current connection.');
    }
    await pc.setRemoteDescription({type: 'answer', sdp: answer.sdp});
    if (this.pc !== pc) return;
    await this.setComparison(pc);
    if (this.pc !== pc) return;
    if (this.channel?.readyState !== 'open')
      this.update({phase: 'connecting', message: 'Connecting directly to the other PC…'});
    this.waitForConnection();
  }

  private async setComparison(pc: RTCPeerConnection) {
    const fingerprint = (sdp?: string) => sdp?.match(/^a=fingerprint:sha-256 ((?:[A-Fa-f0-9]{2}:){31}[A-Fa-f0-9]{2})\r?$/m)?.[1]?.toLowerCase();
    const offer = pc.localDescription?.type === 'offer' ? pc.localDescription : pc.remoteDescription;
    const answer = pc.localDescription?.type === 'answer' ? pc.localDescription : pc.remoteDescription;
    const first = fingerprint(offer?.sdp);
    const second = fingerprint(answer?.sdp);
    if (!first || !second) throw new Error('Missing device identity.');
    const hash = await digest(new TextEncoder().encode(
        `noventra-direct-v1\n${this.session}\n${this.secret}\n${first}\n${second}`));
    if (this.pc === pc) {
      this.update({comparison: hash.slice(0, 16).match(/.{4}/g)!.join(' ')});
      this.comparisonResolved?.(true);
      this.comparisonResolved = null;
    }
  }

  private waitForConnection() {
    const generation = this.generation;
    if (this.channel?.readyState === 'open') return;
    this.connectionTimer = window.setTimeout(() => {
      if (this.generation === generation && this.channel?.readyState !== 'open')
        this.fail('The network blocked the direct connection. Try the same network or use an encrypted file.');
    }, 45 * 1000);
  }

  private bindChannel(channel: RTCDataChannel) {
    const generation = this.generation;
    if (this.channel && this.channel !== channel) { channel.close(); return; }
    this.channel = channel;
    if (channel.label !== 'noventra-transfer-v1' || !channel.ordered) {
      channel.close();
      this.fail('The other browser uses an unsupported transfer channel.');
      return;
    }
    channel.binaryType = 'arraybuffer';
    channel.bufferedAmountLowThreshold = 256 * 1024;
    channel.onopen = () => {
      if (generation !== this.generation || channel !== this.channel) return;
      window.clearTimeout(this.connectionTimer);
      this.update({phase: 'connected', message: 'Compare the device code below on both PCs, then confirm on each device.'});
    };
    channel.onclose = () => {
      if (this.channel === channel && !['sent', 'received', 'closed', 'error'].includes(this.state.phase))
        this.fail('The other browser disconnected. Create new codes, or use a file.');
    };
    channel.onerror = () => {
      if (generation === this.generation && channel === this.channel)
        this.fail('The direct transfer failed. You can use an encrypted file instead.');
    };
    channel.onmessage = event => {
      this.receiveQueue = this.receiveQueue.then(async () => {
        if (generation === this.generation && channel === this.channel)
          await this.onMessage(event.data, generation);
      }).catch(() => {
        if (generation === this.generation && channel === this.channel)
          this.fail('The connection sent an invalid or incomplete transfer. No import was applied.');
      });
    };
  }

  confirm() {
    if (this.state.phase !== 'connected' || !this.state.comparison ||
        this.channel?.readyState !== 'open') return;
    this.update({localConfirmed: true});
    this.channel.send(JSON.stringify({type: 'confirm', session: this.session, comparison: this.state.comparison}));
  }

  private async onMessage(data: unknown, generation: number): Promise<void> {
    if (typeof data === 'string') {
      if (data.length > 2048) throw new Error('Control message limit.');
      const value = JSON.parse(data) as {type: string, session: string, comparison?: string, bytes?: number, hash?: string};
      if (value.session !== this.session) throw new Error('Wrong session.');
      if (value.type === 'confirm') {
        if (!this.state.comparison && !await this.comparisonReady) return;
        if (generation !== this.generation) return;
        if (!this.state.comparison || value.comparison !== this.state.comparison) throw new Error('Identity mismatch.');
        this.update({remoteConfirmed: true});
        return;
      }
      if (!this.state.localConfirmed || !this.state.remoteConfirmed) throw new Error('Devices not approved.');
      if (value.type === 'begin' && !this.state.sender && !this.manifest && this.state.phase === 'connected') {
        if (!Number.isSafeInteger(value.bytes) || value.bytes! < 64 || value.bytes! > MAX_BYTES ||
            typeof value.hash !== 'string' || !/^[0-9a-f]{64}$/.test(value.hash)) throw new Error('Invalid transfer size.');
        this.manifest = {bytes: value.bytes!, hash: value.hash};
        this.update({phase: 'transferring', message: 'Receiving the encrypted transfer…'});
      } else if (value.type === 'end' && this.manifest && this.receivedBytes === this.manifest.bytes) {
        const bytes = new Uint8Array(this.receivedBytes);
        let offset = 0;
        for (const chunk of this.chunks) { bytes.set(chunk, offset); offset += chunk.length; }
        const expectedHash = this.manifest.hash;
        const actualHash = await digest(bytes);
        if (generation !== this.generation) return;
        if (actualHash !== expectedHash) throw new Error('Incomplete transfer.');
        this.chunks = [];
        this.manifest = null;
        this.update({phase: 'received', message: 'Received. Review the data before importing.', progress: 100});
        this.channel?.send(JSON.stringify({type: 'received', session: this.session}));
        await this.received(transferBytesToBase64(bytes), this.secret);
        bytes.fill(0);
      } else if (value.type === 'received' && this.state.sender && this.sent && this.endSent) {
        this.update({phase: 'sent', message: 'Delivered. The receiving PC still needs to review and import it.', progress: 100});
      } else throw new Error('Unexpected message.');
    } else {
      if (!(data instanceof ArrayBuffer) || !this.manifest || this.state.sender ||
          !this.state.localConfirmed || !this.state.remoteConfirmed ||
          data.byteLength === 0 || data.byteLength > CHUNK_BYTES ||
          this.receivedBytes + data.byteLength > this.manifest.bytes) throw new Error('Unexpected data.');
      this.chunks.push(new Uint8Array(data));
      this.receivedBytes += data.byteLength;
      this.update({progress: Math.floor(100 * this.receivedBytes / this.manifest.bytes)});
    }
  }

  get password(): string { return this.secret; }

  async send(base64: string): Promise<void> {
    const channel = this.channel;
    if (!channel || channel.readyState !== 'open' || !this.state.sender || this.sent ||
        !this.state.localConfirmed || !this.state.remoteConfirmed || this.state.phase !== 'connected')
      throw new Error('Confirm both devices before sending.');
    const bytes = transferBase64ToBytes(base64);
    if (bytes.length < 64 || bytes.length > MAX_BYTES) throw new Error('Transfer is too large.');
    const hash = await digest(bytes);
    if (channel !== this.channel || channel.readyState !== 'open')
      throw new Error('Connection closed.');
    this.sent = true;
    this.update({phase: 'transferring', message: 'Sending the encrypted transfer…'});
    channel.send(JSON.stringify({type: 'begin', session: this.session, bytes: bytes.length, hash}));
    try {
      for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
        if (channel !== this.channel || channel.readyState !== 'open') throw new Error('Connection closed.');
        if (channel.bufferedAmount > 1024 * 1024) {
          await new Promise<void>((resolve, reject) => {
            const cleanup = () => { window.clearTimeout(timer); channel.removeEventListener('bufferedamountlow', ready); channel.removeEventListener('close', closed); };
            const ready = () => { cleanup(); resolve(); };
            const closed = () => { cleanup(); reject(new Error('Connection closed.')); };
            const timer = window.setTimeout(closed, 20 * 1000);
            channel.addEventListener('bufferedamountlow', ready, {once: true});
            channel.addEventListener('close', closed, {once: true});
          });
        }
        channel.send(bytes.slice(offset, offset + CHUNK_BYTES));
        this.update({progress: Math.floor(100 * Math.min(offset + CHUNK_BYTES, bytes.length) / bytes.length)});
      }
      this.endSent = true;
      channel.send(JSON.stringify({type: 'end', session: this.session}));
    } catch {
      this.fail('The transfer was interrupted. Create new codes, or use a file.');
    } finally { bytes.fill(0); }
  }
}
