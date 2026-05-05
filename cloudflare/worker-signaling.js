export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/health') {
      return json({ ok: true, service: 'cf-signaling' });
    }

    if (url.pathname === '/register' && request.method === 'POST') {
      const id = crypto.randomUUID();
      await env.PEERS.put(id, JSON.stringify({ createdAt: Date.now() }), { expirationTtl: 86400 });
      return json({ ok: true, peer_id: id });
    }

    if (url.pathname === '/send' && request.method === 'POST') {
      const body = await request.json();
      const { sender, recipient, payload } = body;
      if (!sender || !recipient) return json({ ok: false, error: 'sender and recipient required' }, 400);
      const exists = await env.PEERS.get(recipient);
      if (!exists) return json({ ok: false, error: 'recipient not found' }, 404);
      const key = `msg:${recipient}:${crypto.randomUUID()}`;
      const item = { sender, recipient, payload, ts: Date.now() };
      await env.PEERS.put(key, JSON.stringify(item), { expirationTtl: 600 });
      return json({ ok: true });
    }

    if (url.pathname === '/poll' && request.method === 'GET') {
      const peer_id = url.searchParams.get('peer_id');
      if (!peer_id) return json({ ok: false, error: 'peer_id required' }, 400);
      const list = await env.PEERS.list({ prefix: `msg:${peer_id}:`, limit: 50 });
      const msgs = [];
      for (const k of list.keys) {
        const v = await env.PEERS.get(k.name);
        if (v) msgs.push(JSON.parse(v));
        await env.PEERS.delete(k.name);
      }
      return json({ ok: true, peer_id, messages: msgs });
    }

    return json({ ok: false, error: 'not found' }, 404);
  }
};

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: {
      'content-type': 'application/json',
      'access-control-allow-origin': '*',
      'access-control-allow-methods': 'GET,POST,OPTIONS',
      'access-control-allow-headers': 'content-type'
    }
  });
}
