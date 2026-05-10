require('dotenv').config();
const express    = require('express');
const http       = require('http');
const { Server } = require('socket.io');
const { WebSocketServer } = require('ws');
const path       = require('path');
const cors       = require('cors');

const app    = express();
const server = http.createServer(app);

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../dashboard')));

// ── State ─────────────────────────────────
const state = {
  transmitter : null,
  receivers   : new Map(),
  dashboards  : new Set(),
  packetCount : 0,
  packetTimes : [],
  startTime   : Date.now()
};

function packetRate() {
  const now = Date.now();
  state.packetTimes = state.packetTimes.filter(t => now - t < 1000);
  return state.packetTimes.length;
}

function statusPayload() {
  return {
    transmitterOnline : !!state.transmitter,
    receiverCount     : state.receivers.size,
    dashboardCount    : state.dashboards.size,
    packetCount       : state.packetCount,
    packetRate        : packetRate(),
    uptime            : Math.floor((Date.now() - state.startTime) / 1000),
    receivers         : Array.from(state.receivers.values())
  };
}

// ══════════════════════════════════════════
//  RAW WEBSOCKET — ESP32 connects at /ws
// ══════════════════════════════════════════
const wss = new WebSocketServer({
  server,
  path              : '/ws',
  maxPayload        : 128 * 1024,  // 128KB max packet
  perMessageDeflate : false         // disable compression — lower CPU
});

// Dashboard gets 1 in 6 packets — reduce memory pressure
let dashCounter = 0;

wss.on('connection', (ws, req) => {
  const ip = req.socket.remoteAddress;
  console.log(`[WS] Connected: ${ip}`);

  let role = null, deviceId = null;

  // Welcome message
  ws.send(JSON.stringify({ event: 'connected', data: { msg: 'CyberWalkie ready' } }));

  ws.on('message', (data, isBinary) => {

    // ── Text = registration ───────────────
    if (!isBinary) {
      try {
        let p = JSON.parse(data.toString());
        if (p.event === 'register') p = p.data;
        role     = p.role;
        deviceId = p.deviceId || p.label || ip;

        if (role === 'transmitter') {
          state.transmitter = ws;
          console.log(`[TX] Online: ${deviceId}`);
          ws.send(JSON.stringify({ event: 'registered', data: { role: 'transmitter' } }));

        } else if (role === 'receiver') {
          state.receivers.set(ws, {
            id: deviceId, label: p.label || deviceId, connectedAt: Date.now()
          });
          console.log(`[RX] Online: ${deviceId}`);
          ws.send(JSON.stringify({ event: 'registered', data: { role: 'receiver' } }));
        }

        broadcastStatus();
      } catch(e) {
        // Ignore malformed JSON
      }
      return;
    }

    // ── Binary = raw PCM audio from TX ────
    if (role !== 'transmitter') return;

    state.packetCount++;
    state.packetTimes.push(Date.now());

    // ── Forward to all ESP32 receivers ────
    // Full rate — receivers need every packet for smooth audio
    state.receivers.forEach((info, rxWs) => {
      if (rxWs.readyState === 1) {  // OPEN
        try {
          rxWs.send(data, { binary: true });
        } catch(e) {
          console.error('[RX] Send error:', e.message);
        }
      }
    });

    // ── Forward to dashboard ──────────────
    // Throttled — dashboard only needs subset for visualization
    dashCounter++;
    if (dashCounter % 6 === 0) {
      try {
        io.volatile.to('dashboards').emit('audio_packet', data);
      } catch(e) {}
    }

    // Send metadata to dashboard every packet
    if (dashCounter % 6 === 0) {
      io.volatile.to('dashboards').emit('packet_meta', {
        seq  : state.packetCount,
        ts   : Date.now(),
        size : Buffer.isBuffer(data) ? data.length : data.byteLength,
        rate : packetRate()
      });
    }
  });

  ws.on('close', (code, reason) => {
    console.log(`[WS] Closed: ${deviceId || 'unregistered'} (code: ${code})`);
    if (ws === state.transmitter) {
      state.transmitter = null;
      console.log('[TX] Transmitter offline');
      io.emit('transmitter_offline');
    }
    state.receivers.delete(ws);
    broadcastStatus();
  });

  ws.on('error', err => {
    console.error(`[WS] Error (${deviceId}): ${err.message}`);
  });

  // Ping every 20s — keep Render connection alive
  const keepAlive = setInterval(() => {
    if (ws.readyState === 1) ws.ping();
    else clearInterval(keepAlive);
  }, 20000);

  ws.on('close', () => clearInterval(keepAlive));
});

// ══════════════════════════════════════════
//  SOCKET.IO — Browser dashboard
// ══════════════════════════════════════════
const io = new Server(server, {
  cors             : { origin: '*' },
  maxHttpBufferSize: 128 * 1024,  // 128KB
  pingTimeout      : 10000,
  pingInterval     : 20000
});

function broadcastStatus() {
  io.volatile.to('dashboards').emit('server_status', statusPayload());
}

setInterval(broadcastStatus, 1000);

io.on('connection', (socket) => {
  console.log(`[DASH] Connected: ${socket.id}`);

  socket.on('register', (data) => {
    if (data && data.role === 'dashboard') {
      state.dashboards.add(socket.id);
      socket.join('dashboards');
      socket.emit('registered', { role: 'dashboard', sessionId: socket.id });
      broadcastStatus();
    }
  });

  socket.on('disconnect', () => {
    state.dashboards.delete(socket.id);
    console.log(`[DASH] Disconnected: ${socket.id}`);
    broadcastStatus();
  });
});

// ══════════════════════════════════════════
//  REST ENDPOINTS
// ══════════════════════════════════════════
app.get('/health', (req, res) => res.send('OK'));

app.get('/api/status', (req, res) => res.json(statusPayload()));

app.get('/', (req, res) =>
  res.sendFile(path.join(__dirname, '../dashboard/index.html')));

// ══════════════════════════════════════════
//  MEMORY LOG every 60s
// ══════════════════════════════════════════
setInterval(() => {
  const mb = Math.round(process.memoryUsage().rss / 1024 / 1024);
  const rx = state.receivers.size;
  const pr = packetRate();
  console.log(`[SYS] Mem:${mb}MB | TX:${!!state.transmitter} | RX:${rx} | Rate:${pr}pkt/s`);
}, 60000);

// ══════════════════════════════════════════
//  START
// ══════════════════════════════════════════
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log('');
  console.log('╔══════════════════════════════════════╗');
  console.log('║  CyberWalkie Relay Server             ║');
  console.log(`║  Port     : ${PORT}                      ║`);
  console.log('║  ESP32 WS : /ws                       ║');
  console.log('║  Dashboard: /                         ║');
  console.log('╚══════════════════════════════════════╝');
  console.log('');
});