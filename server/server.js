require('dotenv').config();
const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const cors = require('cors');
const path = require('path');

const app = express();
const server = http.createServer(app);

const io = new Server(server, {
  cors: {
    origin: '*',
    methods: ['GET', 'POST']
  },
  maxHttpBufferSize: 1e6, // 1MB for binary audio
  pingTimeout: 10000,
  pingInterval: 5000
});

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../dashboard')));

// ─── State ───────────────────────────────────────────────
const state = {
  transmitter: null,
  receivers: new Map(),   // socketId → { id, label, connectedAt }
  dashboards: new Set(),
  packetCount: 0,
  packetTimestamps: [],
  startTime: Date.now()
};

// ─── Packet rate calculator ───────────────────────────────
function getPacketRate() {
  const now = Date.now();
  state.packetTimestamps = state.packetTimestamps.filter(t => now - t < 1000);
  return state.packetTimestamps.length;
}

function broadcastStatus() {
  const status = {
    transmitterOnline: !!state.transmitter,
    receiverCount: state.receivers.size,
    dashboardCount: state.dashboards.size,
    packetCount: state.packetCount,
    packetRate: getPacketRate(),
    uptime: Math.floor((Date.now() - state.startTime) / 1000),
    receivers: Array.from(state.receivers.values()).map(r => ({
      id: r.id,
      label: r.label,
      connectedAt: r.connectedAt
    }))
  };
  io.emit('server_status', status);
}

// Broadcast status every second
setInterval(broadcastStatus, 1000);

// ─── Socket.IO ────────────────────────────────────────────
io.on('connection', (socket) => {
  console.log(`[CONNECT] ${socket.id}`);

  // ── Device registration ──
  socket.on('register', (data) => {
    const { role, label, deviceId } = data;

    if (role === 'transmitter') {
      if (state.transmitter) {
        console.log(`[TX] Replacing old transmitter ${state.transmitter}`);
      }
      state.transmitter = socket.id;
      socket.join('transmitters');
      console.log(`[TX] Registered: ${socket.id} (${label || 'unknown'})`);
      socket.emit('registered', { role: 'transmitter', sessionId: socket.id });

    } else if (role === 'receiver') {
      state.receivers.set(socket.id, {
        id: deviceId || socket.id.slice(0, 8),
        label: label || `RX-${socket.id.slice(0, 4)}`,
        connectedAt: Date.now()
      });
      socket.join('receivers');
      console.log(`[RX] Registered: ${socket.id} (${label || 'unknown'})`);
      socket.emit('registered', { role: 'receiver', sessionId: socket.id });

    } else if (role === 'dashboard') {
      state.dashboards.add(socket.id);
      socket.join('dashboards');
      console.log(`[DASH] Registered: ${socket.id}`);
      socket.emit('registered', { role: 'dashboard', sessionId: socket.id });
    }

    broadcastStatus();
  });

  // ── Audio packet relay ──
  socket.on('audio_packet', (packet) => {
    if (socket.id !== state.transmitter) return;

    state.packetCount++;
    state.packetTimestamps.push(Date.now());

    const meta = {
      seq: state.packetCount,
      ts: Date.now(),
      size: packet.byteLength || packet.length || 0
    };

    // Relay binary packet to receivers
    socket.to('receivers').emit('audio_packet', packet);

    // Relay to dashboards (with metadata)
    socket.to('dashboards').emit('audio_packet', packet);
    io.to('dashboards').emit('packet_meta', meta);
  });

  // ── Disconnect ──
  socket.on('disconnect', () => {
    console.log(`[DISCONNECT] ${socket.id}`);

    if (socket.id === state.transmitter) {
      state.transmitter = null;
      io.emit('transmitter_offline');
      console.log('[TX] Transmitter went offline');
    }

    if (state.receivers.has(socket.id)) {
      state.receivers.delete(socket.id);
      console.log(`[RX] Receiver removed: ${socket.id}`);
    }

    state.dashboards.delete(socket.id);
    broadcastStatus();
  });
});

// ─── REST API ────────────────────────────────────────────
app.get('/api/status', (req, res) => {
  res.json({
    transmitterOnline: !!state.transmitter,
    receiverCount: state.receivers.size,
    dashboardCount: state.dashboards.size,
    packetCount: state.packetCount,
    packetRate: getPacketRate(),
    uptime: Math.floor((Date.now() - state.startTime) / 1000),
    receivers: Array.from(state.receivers.values())
  });
});

app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, '../dashboard/index.html'));
});

// ─── Start ────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`\n🔥 CyberWalkie Server running on port ${PORT}`);
  console.log(`📡 Dashboard: http://localhost:${PORT}`);
  console.log(`🔑 Relay active\n`);
});
