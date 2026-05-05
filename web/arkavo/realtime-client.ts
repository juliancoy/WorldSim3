/* eslint-disable @typescript-eslint/no-explicit-any */
export type PeerId = string;

export interface ArkavoHelloIce {
  stun?: string[];
  turn?: string[];
  username?: string;
  credential?: string;
}

export interface ArkavoHelloMessage {
  type: 'hello';
  peerId: string;
  ice?: ArkavoHelloIce;
}

export interface ArkavoJoinedMessage {
  type: 'joined';
  roomId?: string;
  peers?: string[];
}

export interface ArkavoPeerJoinedMessage {
  type: 'peer-joined';
  peerId: string;
}

export interface ArkavoPeerLeftMessage {
  type: 'peer-left';
  peerId: string;
}

export interface ArkavoSignalMessage {
  type: 'signal';
  fromPeerId: string;
  targetPeerId?: string | null;
  payload: unknown;
}

export type ArkavoInboundMessage =
  | ArkavoHelloMessage
  | ArkavoJoinedMessage
  | ArkavoPeerJoinedMessage
  | ArkavoPeerLeftMessage
  | ArkavoSignalMessage;

export interface FileMeta {
  transferId: string;
  name: string;
  mimeType: string;
  size: number;
}

export interface ReceivedFile {
  peerId: string;
  meta: FileMeta;
  blob: Blob;
}

export interface RealtimeClientOptions {
  roomId: string;
  signalingUrl?: string;
  rtcConfig?: RTCConfiguration;
  reconnectBaseMs?: number;
  reconnectMaxMs?: number;
  iceCandidatePoolSize?: number;
  chunkSize?: number;
  log?: (...args: unknown[]) => void;
}

interface PeerState {
  pc: RTCPeerConnection;
  dc?: RTCDataChannel;
  makingOffer: boolean;
  ignoreOffer: boolean;
  polite: boolean;
  isReady: boolean;
}

interface InboundTransferState {
  meta: FileMeta;
  receivedBytes: number;
  chunks: Uint8Array[];
}

type ControlMessage =
  | { type: 'file-meta'; transferId: string; name: string; mimeType: string; size: number }
  | { type: 'file-end'; transferId: string };

const DEFAULT_SIGNALING_URL = 'wss://signaling.arkavo.org/';
const DEFAULT_STUN_FALLBACK = 'stun:stun.l.google.com:19302';

export class RealtimeClient {
  private readonly roomId: string;
  private readonly signalingUrl: string;
  private readonly reconnectBaseMs: number;
  private readonly reconnectMaxMs: number;
  private readonly chunkSize: number;
  private readonly log: (...args: unknown[]) => void;
  private readonly peers = new Map<PeerId, PeerState>();
  private readonly inboundTransfers = new Map<string, InboundTransferState>();

  private ws: WebSocket | null = null;
  private wsConnected = false;
  private wsIntentionalClose = false;
  private reconnectAttempt = 0;
  private reconnectTimer: number | null = null;
  private currentIce: ArkavoHelloIce | null = null;

  public selfPeerId = '';

  public onPeerJoined: (peerId: PeerId) => void = () => {};
  public onPeerLeft: (peerId: PeerId) => void = () => {};
  public onPeerConnectionState: (peerId: PeerId, state: RTCPeerConnectionState) => void = () => {};
  public onChannelState: (peerId: PeerId, state: RTCDataChannelState) => void = () => {};
  public onFileReceived: (file: ReceivedFile) => void = () => {};
  public onError: (err: unknown) => void = () => {};

  constructor(opts: RealtimeClientOptions) {
    this.roomId = opts.roomId;
    this.signalingUrl = opts.signalingUrl ?? DEFAULT_SIGNALING_URL;
    this.reconnectBaseMs = opts.reconnectBaseMs ?? 500;
    this.reconnectMaxMs = opts.reconnectMaxMs ?? 15000;
    this.chunkSize = opts.chunkSize ?? 64 * 1024;
    this.log = opts.log ?? (() => undefined);
  }

  public connect(): void {
    this.wsIntentionalClose = false;
    this.openWs();
  }

  public disconnect(): void {
    this.wsIntentionalClose = true;
    if (this.reconnectTimer != null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
    this.wsConnected = false;
    for (const peerId of this.peers.keys()) this.teardownPeer(peerId);
  }

  public listPeers(): string[] {
    return Array.from(this.peers.keys());
  }

  public async sendFile(peerId: string, file: File): Promise<void> {
    const state = this.peers.get(peerId);
    if (!state?.dc || state.dc.readyState !== 'open') {
      throw new Error(`Data channel not open for peer ${peerId}`);
    }
    const transferId = crypto.randomUUID();
    const meta: ControlMessage = {
      type: 'file-meta',
      transferId,
      name: file.name,
      mimeType: file.type || 'application/octet-stream',
      size: file.size,
    };
    state.dc.send(JSON.stringify(meta));

    let offset = 0;
    while (offset < file.size) {
      const slice = file.slice(offset, Math.min(offset + this.chunkSize, file.size));
      const buf = await slice.arrayBuffer();
      state.dc.send(buf);
      offset += slice.size;
      if (state.dc.bufferedAmount > 4 * this.chunkSize) {
        await this.waitForBufferedAmountLow(state.dc);
      }
    }

    const end: ControlMessage = { type: 'file-end', transferId };
    state.dc.send(JSON.stringify(end));
  }

  private openWs(): void {
    if (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    this.log('ws: connecting', this.signalingUrl);
    const ws = new WebSocket(this.signalingUrl);
    this.ws = ws;

    ws.onopen = () => {
      this.wsConnected = true;
      this.reconnectAttempt = 0;
      this.log('ws: open');
    };

    ws.onmessage = (ev) => {
      try {
        const raw = JSON.parse(String(ev.data));
        if (!this.isInboundMessage(raw)) return;
        void this.handleInboundMessage(raw);
      } catch (err) {
        this.safeError(err);
      }
    };

    ws.onerror = (ev) => {
      this.safeError(ev);
    };

    ws.onclose = () => {
      this.wsConnected = false;
      this.log('ws: closed');
      for (const peerId of this.peers.keys()) this.teardownPeer(peerId);
      this.inboundTransfers.clear();
      if (this.wsIntentionalClose) return;
      this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer != null) return;
    const jitter = Math.floor(Math.random() * 250);
    const wait = Math.min(this.reconnectMaxMs, this.reconnectBaseMs * 2 ** this.reconnectAttempt) + jitter;
    this.reconnectAttempt += 1;
    this.log('ws: reconnect in', wait, 'ms');
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null;
      this.openWs();
    }, wait);
  }

  private async handleInboundMessage(msg: ArkavoInboundMessage): Promise<void> {
    switch (msg.type) {
      case 'hello': {
        this.selfPeerId = msg.peerId;
        this.currentIce = msg.ice ?? null;
        this.sendWs({ type: 'join', roomId: this.roomId });
        return;
      }
      case 'joined': {
        const peers = Array.isArray(msg.peers) ? msg.peers.filter((p) => typeof p === 'string') : [];
        for (const peerId of peers) {
          if (!peerId || peerId === this.selfPeerId) continue;
          await this.ensurePeer(peerId, this.shouldInitiateForPeer(peerId));
        }
        return;
      }
      case 'peer-joined': {
        if (!msg.peerId || msg.peerId === this.selfPeerId) return;
        await this.ensurePeer(msg.peerId, this.shouldInitiateForPeer(msg.peerId));
        this.onPeerJoined(msg.peerId);
        return;
      }
      case 'peer-left': {
        if (!msg.peerId) return;
        this.teardownPeer(msg.peerId);
        this.onPeerLeft(msg.peerId);
        return;
      }
      case 'signal': {
        if (!msg.fromPeerId || msg.fromPeerId === this.selfPeerId) return;
        await this.handleSignal(msg.fromPeerId, msg.payload);
        return;
      }
      default:
        return;
    }
  }

  private async ensurePeer(peerId: string, initiate: boolean): Promise<PeerState> {
    const existing = this.peers.get(peerId);
    if (existing) return existing;

    const pc = new RTCPeerConnection(this.buildIceConfig());
    const state: PeerState = {
      pc,
      makingOffer: false,
      ignoreOffer: false,
      polite: this.selfPeerId < peerId,
      isReady: false,
    };
    this.peers.set(peerId, state);

    pc.onicecandidate = ({ candidate }) => {
      if (!candidate) return;
      this.sendSignal(peerId, { candidate: candidate.toJSON() });
    };

    pc.onconnectionstatechange = () => {
      this.onPeerConnectionState(peerId, pc.connectionState);
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed' || pc.connectionState === 'disconnected') {
        state.isReady = false;
      }
    };

    pc.ondatachannel = (ev) => {
      this.attachDataChannel(peerId, state, ev.channel);
    };

    pc.onnegotiationneeded = async () => {
      try {
        state.makingOffer = true;
        await pc.setLocalDescription();
        this.sendSignal(peerId, { description: pc.localDescription });
      } catch (err) {
        this.safeError(err);
      } finally {
        state.makingOffer = false;
      }
    };

    if (initiate) {
      const dc = pc.createDataChannel('files', { ordered: true });
      this.attachDataChannel(peerId, state, dc);
      await pc.setLocalDescription(await pc.createOffer());
      this.sendSignal(peerId, { description: pc.localDescription });
    }

    return state;
  }

  private async handleSignal(fromPeerId: string, payload: unknown): Promise<void> {
    if (!this.isSignalPayload(payload)) return;

    const state = await this.ensurePeer(fromPeerId, false);
    const pc = state.pc;

    try {
      if (payload.description) {
        const desc = payload.description;
        const offerCollision =
          desc.type === 'offer' && (state.makingOffer || pc.signalingState !== 'stable');

        state.ignoreOffer = !state.polite && offerCollision;
        if (state.ignoreOffer) return;

        await pc.setRemoteDescription(desc);
        if (desc.type === 'offer') {
          await pc.setLocalDescription(await pc.createAnswer());
          this.sendSignal(fromPeerId, { description: pc.localDescription });
        }
      } else if (payload.candidate) {
        try {
          await pc.addIceCandidate(payload.candidate);
        } catch (err) {
          if (!state.ignoreOffer) throw err;
        }
      }
    } catch (err) {
      this.safeError(err);
    }
  }

  private attachDataChannel(peerId: string, state: PeerState, dc: RTCDataChannel): void {
    state.dc = dc;
    dc.binaryType = 'arraybuffer';

    let activeTransferId = '';

    dc.onopen = () => {
      state.isReady = true;
      this.onChannelState(peerId, dc.readyState);
    };
    dc.onclose = () => {
      state.isReady = false;
      this.onChannelState(peerId, dc.readyState);
    };
    dc.onerror = (ev) => {
      this.safeError(ev);
    };

    dc.onmessage = async (ev) => {
      if (typeof ev.data === 'string') {
        let msg: unknown;
        try {
          msg = JSON.parse(ev.data);
        } catch {
          return;
        }
        if (!this.isControlMessage(msg)) return;

        if (msg.type === 'file-meta') {
          activeTransferId = msg.transferId;
          this.inboundTransfers.set(msg.transferId, {
            meta: {
              transferId: msg.transferId,
              name: msg.name,
              mimeType: msg.mimeType,
              size: msg.size,
            },
            receivedBytes: 0,
            chunks: [],
          });
          return;
        }

        if (msg.type === 'file-end') {
          const transfer = this.inboundTransfers.get(msg.transferId);
          if (!transfer) return;
          const blob = new Blob(transfer.chunks, { type: transfer.meta.mimeType || 'application/octet-stream' });
          this.inboundTransfers.delete(msg.transferId);
          this.onFileReceived({ peerId, meta: transfer.meta, blob });
          activeTransferId = '';
        }
        return;
      }

      const arr = this.toUint8Array(ev.data);
      if (!arr || !activeTransferId) return;
      const transfer = this.inboundTransfers.get(activeTransferId);
      if (!transfer) return;
      transfer.chunks.push(arr);
      transfer.receivedBytes += arr.byteLength;
    };
  }

  private shouldInitiateForPeer(peerId: string): boolean {
    if (!this.selfPeerId) return true;
    return this.selfPeerId < peerId;
  }

  private toUint8Array(data: unknown): Uint8Array | null {
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (data instanceof Blob) {
      // blob messages should not happen with binaryType=arraybuffer, ignore for safety
      return null;
    }
    return null;
  }

  private buildIceConfig(): RTCConfiguration {
    const servers: RTCIceServer[] = [];
    const helloIce = this.currentIce;

    if (helloIce?.turn?.length) {
      servers.push({
        urls: helloIce.turn,
        username: helloIce.username || undefined,
        credential: helloIce.credential || undefined,
      });
    }

    const hasAnyStun = servers.some((s) => {
      const urls = Array.isArray(s.urls) ? s.urls : [s.urls];
      return urls.some((u) => String(u).startsWith('stun:'));
    });

    if (helloIce?.stun?.length) {
      servers.push({ urls: helloIce.stun });
    } else if (!hasAnyStun) {
      servers.push({ urls: [DEFAULT_STUN_FALLBACK] });
    }

    return {
      iceServers: servers,
      iceCandidatePoolSize: 4,
      bundlePolicy: 'max-bundle',
    };
  }

  private sendSignal(targetPeerId: string | null, payload: unknown): void {
    this.sendWs({
      type: 'signal',
      targetPeerId,
      payload,
    });
  }

  private sendWs(msg: unknown): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify(msg));
  }

  private teardownPeer(peerId: string): void {
    const state = this.peers.get(peerId);
    if (!state) return;
    try {
      state.dc?.close();
      state.pc.close();
    } catch {
      // no-op
    }
    this.peers.delete(peerId);
  }

  private waitForBufferedAmountLow(dc: RTCDataChannel): Promise<void> {
    return new Promise((resolve) => {
      dc.bufferedAmountLowThreshold = this.chunkSize;
      const onLow = () => {
        dc.removeEventListener('bufferedamountlow', onLow);
        resolve();
      };
      dc.addEventListener('bufferedamountlow', onLow);
    });
  }

  private safeError(err: unknown): void {
    this.log('realtime error', err);
    this.onError(err);
  }

  private isInboundMessage(v: unknown): v is ArkavoInboundMessage {
    if (!v || typeof v !== 'object') return false;
    const t = (v as any).type;
    if (typeof t !== 'string') return false;
    return t === 'hello' || t === 'joined' || t === 'peer-joined' || t === 'peer-left' || t === 'signal';
  }

  private isSignalPayload(v: unknown): v is { description?: RTCSessionDescriptionInit; candidate?: RTCIceCandidateInit } {
    if (!v || typeof v !== 'object') return false;
    const p = v as any;
    const hasDesc = !!p.description && typeof p.description === 'object' && typeof p.description.type === 'string';
    const hasCand = !!p.candidate && typeof p.candidate === 'object' && typeof p.candidate.candidate === 'string';
    return hasDesc || hasCand;
  }

  private isControlMessage(v: unknown): v is ControlMessage {
    if (!v || typeof v !== 'object') return false;
    const t = (v as any).type;
    if (t === 'file-meta') {
      return typeof (v as any).transferId === 'string' && typeof (v as any).name === 'string' && typeof (v as any).size === 'number';
    }
    if (t === 'file-end') {
      return typeof (v as any).transferId === 'string';
    }
    return false;
  }
}
