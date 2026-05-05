import { RealtimeClient } from './realtime-client';

const roomInput = document.getElementById('room') as HTMLInputElement;
const connectBtn = document.getElementById('connect') as HTMLButtonElement;
const disconnectBtn = document.getElementById('disconnect') as HTMLButtonElement;
const peersEl = document.getElementById('peers') as HTMLUListElement;
const fileInput = document.getElementById('file') as HTMLInputElement;
const targetPeerInput = document.getElementById('targetPeer') as HTMLInputElement;
const sendBtn = document.getElementById('sendFile') as HTMLButtonElement;
const logsEl = document.getElementById('logs') as HTMLPreElement;

let client: RealtimeClient | null = null;

function log(...args: unknown[]) {
  logsEl.textContent += args.map(String).join(' ') + '\n';
  logsEl.scrollTop = logsEl.scrollHeight;
}

function refreshPeers() {
  if (!client) return;
  peersEl.innerHTML = '';
  for (const p of client.listPeers()) {
    const li = document.createElement('li');
    li.textContent = p;
    peersEl.appendChild(li);
  }
}

connectBtn.onclick = () => {
  if (client) client.disconnect();
  client = new RealtimeClient({ roomId: roomInput.value.trim() || 'default-room', log });

  client.onPeerJoined = (peerId) => {
    log('peer joined:', peerId);
    refreshPeers();
  };
  client.onPeerLeft = (peerId) => {
    log('peer left:', peerId);
    refreshPeers();
  };
  client.onPeerConnectionState = (peerId, state) => {
    log('pc state', peerId, state);
    refreshPeers();
  };
  client.onChannelState = (peerId, state) => {
    log('dc state', peerId, state);
  };
  client.onFileReceived = ({ peerId, meta, blob }) => {
    log('file received from', peerId, meta.name, meta.size, 'bytes');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = meta.name;
    a.textContent = `download ${meta.name} from ${peerId}`;
    document.body.appendChild(a);
    document.body.appendChild(document.createElement('br'));
  };
  client.onError = (err) => log('error:', String(err));

  client.connect();
  log('connecting...');
};

disconnectBtn.onclick = () => {
  client?.disconnect();
  log('disconnected');
};

sendBtn.onclick = async () => {
  if (!client) {
    log('not connected');
    return;
  }
  const peerId = targetPeerInput.value.trim();
  const file = fileInput.files?.[0];
  if (!peerId || !file) {
    log('peer and file required');
    return;
  }
  try {
    await client.sendFile(peerId, file);
    log('file sent to', peerId, file.name, file.size);
  } catch (err) {
    log('send failed:', String(err));
  }
};
