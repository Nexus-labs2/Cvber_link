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

// ══════════════════════════════════════════════════════
//  RAW WEBSOCKET  — ESP32 connects here at path /ws
// ══════════════════════════════════════════════════════
const wss = new WebSocketServer({ server, path: '/ws' });

wss.on('connection', (ws, req) => {
  console.log('[RAW WS] ESP32 connected from ' + req.socket.remoteAddress);
  let role = null, deviceId = null;

  ws.send(JSON.stringify({ event: 'connected', data: { msg: 'CYBERLINK-7 ready' } }));

  ws.on('message', (data, isBinary) => {
    if (!isBinary) {
      try {
        let p = JSON.parse(data.toString());
        if (p.event === 'register') p = p.data;
        role     = p.role;
        deviceId = p.deviceId || p.label || 'unknown';

        if (role === 'transmitter') {
          state.transmitter = ws;
          console.log('[TX] Registered: ' + deviceId);
          ws.send(JSON.stringify({ event: 'registered', data: { role: 'transmitter' } }));
        } else if (role === 'receiver') {
          state.receivers.set(ws, { id: deviceId, label: p.label || deviceId, connectedAt: Date.now() });
          console.log('[RX] Registered: ' + deviceId);
          ws.send(JSON.stringify({ event: 'registered', data: { role: 'receiver' } }));
        }
        broadcastStatus();
      } catch(e) {}
      return;
    }

    if (role === 'transmitter') {
      state.packetCount++;
      state.packetTimes.push(Date.now());
      state.receivers.forEach((info, rxWs) => {
        if (rxWs.readyState === 1) rxWs.send(data, { binary: true });
      });
      io.to('dashboards').emit('audio_packet', data);
      io.to('dashboards').emit('packet_meta', {
        seq: state.packetCount, ts: Date.now(), size: data.length
      });
    }
  });

  ws.on('close', () => {
    console.log('[RAW WS] Disconnected: ' + (deviceId || 'unknown'));
    if (ws === state.transmitter) { state.transmitter = null; io.emit('transmitter_offline'); }
    state.receivers.delete(ws);
    broadcastStatus();
  });

  ws.on('error', (err) => console.error('[RAW WS] Error: ' + err.message));

  const ping = setInterval(() => { if (ws.readyState === 1) ws.ping(); }, 25000);
  ws.on('close', () => clearInterval(ping));
});

// ══════════════════════════════════════════════════════
//  SOCKET.IO — Browser dashboard connects here
// ══════════════════════════════════════════════════════
const io = new Server(server, {
  cors             : { origin: '*' },
  maxHttpBufferSize: 1e6,
  pingTimeout      : 10000,
  pingInterval     : 25000
});

function broadcastStatus() {
  io.to('dashboards').emit('server_status', statusPayload());
}
setInterval(broadcastStatus, 1000);

io.on('connection', (socket) => {
  console.log('[SOCKET.IO] Connected: ' + socket.id);
  socket.on('register', (data) => {
    if (data.role === 'dashboard') {
      state.dashboards.add(socket.id);
      socket.join('dashboards');
      socket.emit('registered', { role: 'dashboard', sessionId: socket.id });
      console.log('[DASH] Registered: ' + socket.id);
      broadcastStatus();
    }
  });
  socket.on('disconnect', () => {
    state.dashboards.delete(socket.id);
    broadcastStatus();
  });
});

// ══════════════════════════════════════════════════════
//  REST ENDPOINTS
// ══════════════════════════════════════════════════════
app.get('/health',     (req, res) => res.send('OK'));
app.get('/api/status', (req, res) => res.json(statusPayload()));
app.get('/',           (req, res) => res.sendFile(path.join(__dirname, '../dashboard/index.html')));

// ══════════════════════════════════════════════════════
//  START SERVER
// ══════════════════════════════════════════════════════
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log('\n╔══════════════════════════════════════╗');
  console.log('║  CYBERLINK-7 Relay Server             ║');
  console.log('║  Port     : ' + PORT + '                        ║');
  console.log('║  ESP32 WS : ws://host/ws              ║');
  console.log('║  Dashboard: http://host/              ║');
  console.log('╚══════════════════════════════════════╝\n');
});