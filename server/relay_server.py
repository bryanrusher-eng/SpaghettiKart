#!/usr/bin/env python3
"""
Servidor de rele para Spaghetti Kart Online.

Permite jugar por internet sin abrir puertos ni usar VPN: el anfitrion crea una sala
(el servidor le da un codigo corto) y los clientes entran con ese codigo. El servidor
reenvia paquetes UDP entre el anfitrion y cada cliente. Durante una carrera tambien hace de AGREGADOR de mandos:
recibe el mando de cada jugador, arma el fotograma de cada tick y lo difunde a todos (el juego es lockstep
determinista: todas las PCs simulan lo mismo con los mismos mandos). El servidor NO calcula fisicas.

Uso:
    python3 relay_server.py [--port 7778] [--verbose]

Hace falta abrir el puerto UDP elegido en el firewall del servidor.
"""

import argparse
import asyncio
import collections
import logging
import random
import struct
import sys
import time

MAGIC = 0x3152464D  # "MFR1" en little endian (distinto del magic del juego, "MKN1")
ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # sin 0/O/1/I para evitar confusiones
CODE_LEN = 5

# tipos de paquete
R_CREATE = 1        # anfitrion -> servidor: crear sala
R_CREATED = 2       # servidor -> anfitrion: codigo de la sala
R_JOIN = 3          # cliente -> servidor: unirse con codigo
R_JOINED = 4        # servidor -> cliente: ok (u8) [+ motivo si falla]
R_DATA_UP = 5       # cliente -> servidor: paquete del juego (se reenvia al anfitrion)
R_DATA_FROM = 6     # servidor -> anfitrion: paquete del juego + id de cliente
R_DATA_TO = 7       # anfitrion -> servidor: id de cliente + paquete del juego
R_DATA_DOWN = 8     # servidor -> cliente: paquete del juego
R_KEEPALIVE = 9     # ambos -> servidor: sigo aqui (mantiene el NAT abierto)
R_CLOSE = 10        # anfitrion -> servidor: cerrar sala / cliente: salir
R_ROOM_CLOSED = 11  # servidor -> cliente: la sala se cerro
R_ERROR = 12        # servidor -> cualquiera: motivo
# --- agregador de mandos (carrera) ---
R_PING = 13         # servidor -> jugador: medir RTT (ocho bytes de marca, se devuelven tal cual)
R_PONG = 14         # jugador -> servidor
R_AGG_BEGIN = 15    # anfitrion -> servidor: empieza una carrera. u8 n, n x (u8 slot, u16 id de cliente)
R_AGG_PARAMS = 16   # servidor -> jugador: u8 retraso de entrada (ticks), u16 mascara de humanos
R_AGG_INPUT = 17    # jugador -> servidor: u32 ack, u32 primer tick, u8 n, n x 4 bytes de mando
R_AGG_FRAME = 18    # servidor -> jugador: u32 primer tick, u8 n, n x (4 bytes por humano, por slot)
R_AGG_LEAVE = 22    # jugador -> servidor: salgo de la carrera (dejad de esperar mis mandos)
R_AGG_DROPPED = 21  # servidor -> jugador: se te dio por caido de la carrera (sal de ella)
R_AGG_LEFT = 19     # servidor -> anfitrion: u8 slot que dejo de responder
R_AGG_END = 20      # anfitrion -> servidor: fin de la carrera

MAX_PAYLOAD = 1300
MAX_CLIENTS_PER_ROOM = 15
MAX_ROOMS_PER_IP = 3
MAX_ROOMS = 500
ROOM_TIMEOUT = 15.0   # segundos sin noticias del anfitrion
CLIENT_TIMEOUT = 15.0
CREATE_COOLDOWN = 1.0  # segundos minimos entre creaciones desde una misma IP

TICK_MS = 1000.0 / 30.0
NEUTRAL = b"\x00\x00\x00\x00"
AGG_DEADLINE = 0.120    # s: un mando que tarda mas que esto tras el ultimo fotograma se sustituye por el anterior
AGG_SILENT = 0.5        # s sin senales de un jugador: ya no se le espera (se rellena su mando) hasta que vuelva
AGG_DROP_AFTER = 3.0    # s sin noticias de un jugador en carrera: se da por desconectado
AGG_BARRIER = 90.0      # s maximos esperando a que todos terminen de cargar la pista
AGG_MAX_AHEAD = 100      # ticks: el rele no se adelanta mas que esto al jugador que mas atrasado va (el anillo del cliente es de 256)
AGG_DUP = 2            # copias de cada fotograma que se mandan a cada jugador (cubre paquetes perdidos)
MIN_DELAY = 2
MAX_DELAY = 16
# modo de latencia -> (multiplicador del jitter, ms fijos de margen)
LAT_MODES = {0: (3.0, 24.0), 1: (2.0, 16.0), 2: (1.0, 0.0)}

log = logging.getLogger("relay")


def pack(ptype, body=b""):
    return struct.pack("<IB", MAGIC, ptype) + body


def pstr(s):
    b = s.encode("utf-8")[:32]
    return bytes([len(b)]) + b


def read_str(data, off):
    if off >= len(data):
        return None, off
    n = data[off]
    if off + 1 + n > len(data):
        return None, off
    return data[off + 1:off + 1 + n].decode("utf-8", "replace"), off + 1 + n


class Room:
    def __init__(self, code, host_addr):
        self.code = code
        self.host = host_addr
        self.host_seen = time.monotonic()
        self.clients = {}   # id -> [addr, last_seen]
        self.by_addr = {}   # addr -> id
        self.next_id = 1
        self.agg = None     # agregador de la carrera en curso (o None)


class Agg:
    """Estado de una carrera en una sala: reune los mandos y emite los fotogramas."""

    def __init__(self, relay, humans, delays, now):
        self.humans = dict(humans)               # slot -> direccion
        self.slot_of = {a: s for s, a in humans.items()}
        self.slots = sorted(humans)
        self.delay = dict(delays)                # slot -> retraso de entrada (ticks)
        self.inp = {}                            # tick -> {slot: 4 bytes}
        self.last_pad = {s: NEUTRAL for s in self.slots}
        self.next = 0
        self.frames = {}                         # tick -> 4 bytes x len(slots)
        self.ack = {s: 0 for s in self.slots}
        self.last_sent = {s: 0.0 for s in self.slots}
        self.heard = {s: now for s in self.slots}
        self.started = set()
        self.hi = {s: -1 for s in self.slots}    # slot -> tick mas alto con mando REAL recibido
        self.dropped = set()
        self.skip_until = {}                     # slot -> tick hasta el que no se le espera (iba tarde)
        self.released = False
        self.deadline = AGG_DEADLINE             # plazo actual para esperar un mando (se adapta al ping)
        self.fill_log_t = now
        self.fill_logged = 0
        self.begin = now
        self.last_emit = now
        # contadores para el registro final
        self.n_emit = 0
        self.n_fill = 0
        self.fill_by_slot = {s: 0 for s in self.slots}
        self.max_frames = max(1, min(60, (1100 - 9) // (4 * max(1, len(self.slots)))))   # fotogramas por paquete (cabe en un MTU)
        self.notified = {}                       # slot -> ultimo aviso de "te diste de baja" enviado
        self.behind_since = {}                   # slot -> instante desde el que va mas de AGG_MAX_AHEAD ticks atrasado
        self.params = {}       # slot -> paquete R_AGG_PARAMS (se reenvia hasta que el jugador empieza)
        self.params_t = {}

    # --- entrada de mandos ---
    def on_input(self, relay, addr, body, now):
        s = self.slot_of.get(addr)
        if s is None or len(body) < 9:
            return
        if s in self.dropped:
            self.notify_dropped(relay, s, now)     # sigue vivo pero ya no esta en la carrera: que salga
            return
        ack, first, count = struct.unpack_from("<IIB", body, 0)
        if count == 0 or len(body) < 9 + 4 * count:
            return
        self.heard[s] = now
        self.hi[s] = max(self.hi[s], first + count - 1)
        if ack > self.ack[s]:
            self.ack[s] = ack
        self.started.add(s)
        if s in self.skip_until and first + count - 1 >= self.next:
            del self.skip_until[s]                 # ya va al dia: se vuelve a esperar su mando
        for k in range(count):
            t = first + k
            if t < self.next or t > self.next + 600:
                continue
            slot_inputs = self.inp.setdefault(t, {})
            if s not in slot_inputs:
                slot_inputs[s] = body[9 + 4 * k:13 + 4 * k]
        self.try_emit(relay, now)

    def fill_pad(self, s):
        """Mando que se pone a un jugador que no llego a tiempo: conserva los botones, pero con el stick en el centro
        (repetir el stick girado haria que su kart diera vueltas sin parar)."""
        p = self.last_pad[s]
        return p[:2] + b"\x00\x00"

    # --- emision ---
    def try_release(self, now):
        if self.released:
            return True
        if all(s in self.started or s in self.dropped for s in self.slots):
            self.released = True
            self.last_emit = now
            return True
        return False

    def try_emit(self, relay, now):
        if not self.try_release(now):
            return
        if len(self.dropped) >= len(self.slots):
            return                                 # nadie a quien esperar ni a quien enviar: no se emite nada
        for _ in range(1000):                      # tope por llamada (un bucle sin salida colgaria todo el rele)
            T = self.next
            if T - self.floor_ack() >= AGG_MAX_AHEAD:
                return                             # un jugador va muy atrasado: se espera a que reciba lo pendiente
            if T > self.emit_limit():
                return                             # nadie ha mandado mandos reales hasta aqui: no se inventan fotogramas
            d = self.inp.get(T, {})
            pads = []
            missing = []
            for s in self.slots:
                if s in self.dropped or T < self.delay[s]:
                    pads.append(NEUTRAL)       # caido o aun en su retraso inicial: mando neutro
                elif s in d:
                    pads.append(d[s])
                elif now - self.heard[s] > AGG_SILENT:
                    pads.append(self.fill_pad(s))   # lleva un rato sin dar senales: no se le espera, se rellena su mando
                else:
                    pads.append(None)
                    missing.append(s)
            if missing:
                if now - self.last_emit < self.deadline:
                    return
                for i, s in enumerate(self.slots):
                    if pads[i] is None:
                        pads[i] = self.fill_pad(s)         # solo este tick: el jugador atrasado se pone al dia esperandole
                        self.fill_by_slot[s] += 1
                        self.n_fill += 1
            for i, s in enumerate(self.slots):
                if s not in self.dropped and T >= self.delay[s]:
                    self.last_pad[s] = pads[i]
            self.frames[T] = b"".join(pads)
            self.inp.pop(T, None)
            self.next = T + 1
            self.last_emit = now
            self.n_emit += 1
            self.send_frame(relay, T, now)
            old = T - 600
            if old in self.frames:
                self.frames.pop(old, None)

    def emit_limit(self):
        """Ultimo tick que se puede emitir: el mando real mas alto recibido (los primeros ticks salen neutros sin mando)."""
        live = [self.hi[s] for s in self.slots if s not in self.dropped]
        return max(max(live) if live else -1, max(self.delay.values()) - 1)

    def floor_ack(self):
        """Fotograma mas antiguo que aun necesita algun jugador conectado y ya iniciado."""
        acks = [self.ack[s] for s in self.slots if s not in self.dropped and s in self.started]
        return min(acks) if acks else self.next

    def frame_packet(self, start, end):
        end = min(end, start + self.max_frames - 1)
        body = struct.pack("<IB", start, end - start + 1) + b"".join(self.frames[t] for t in range(start, end + 1))
        return pack(R_AGG_FRAME, body)

    def send_frame(self, relay, T, now):
        for s in self.slots:
            if s in self.dropped:
                continue
            start = max(min(self.ack[s], T - 1), 0)
            pkt = self.frame_packet(start, T)
            for _ in range(AGG_DUP):
                relay.send_leg(self.humans[s], pkt)
            self.last_sent[s] = now

    # --- temporizador: plazos, reenvios y caidas ---
    def on_timer(self, relay, now):
        live = [self.humans[s] for s in self.slots if s not in self.dropped]
        worst = max((relay.rtt_of(a)[0] + 3 * relay.rtt_of(a)[1] for a in live), default=0.0)
        self.deadline = max(AGG_DEADLINE, min(0.25, worst / 1000.0 + 0.04))
        if self.released and now - self.fill_log_t >= 10.0:
            self.fill_log_t = now
            if self.n_fill != self.fill_logged:
                log.info("agregador: rellenos por plazo hasta ahora %s (plazo %.0f ms)",
                         {s: n for s, n in self.fill_by_slot.items() if n}, self.deadline * 1000)
                self.fill_logged = self.n_fill
        if not self.released and now - self.begin > AGG_BARRIER:
            for s in self.slots:
                if s not in self.started:
                    self.drop(relay, s)
        for s in self.slots:
            if s not in self.started and s not in self.dropped and now - self.params_t.get(s, now) >= 0.5:
                self.params_t[s] = now
                relay.send(self.humans[s], self.params[s])
        self.try_emit(relay, now)
        if not self.released:
            return
        for s in self.slots:
            if s in self.dropped:
                continue
            if now - self.heard[s] > AGG_DROP_AFTER:
                self.drop(relay, s)
                continue
            if self.ack[s] < self.next and now - self.last_sent[s] >= 0.06:
                relay.send_leg(self.humans[s], self.frame_packet(self.ack[s], self.next - 1))
                self.last_sent[s] = now
            # jugador que no avanza (congelado): si lleva demasiado atrasado, se da por caido y no frena a los demas
            if s in self.started and self.next - self.ack[s] >= AGG_MAX_AHEAD:
                since = self.behind_since.setdefault(s, now)
                if now - since > AGG_DROP_AFTER:
                    self.drop(relay, s)
            else:
                self.behind_since.pop(s, None)
        self.try_emit(relay, now)

    def notify_dropped(self, relay, s, now):
        if now - self.notified.get(s, 0.0) < 0.5 or s not in self.humans:
            return
        self.notified[s] = now
        pkt = pack(R_AGG_DROPPED, b"")
        relay.send(self.humans[s], pkt)
        relay.send(self.humans[s], pkt)

    def drop(self, relay, s):
        if s in self.dropped:
            return
        self.dropped.add(s)
        log.info("agregador: el jugador de slot %d dejo de responder", s)
        self.notify_dropped(relay, s, time.monotonic())
        host_addr = self.humans.get(0)
        if host_addr is not None and s != 0:
            relay.send(host_addr, pack(R_AGG_LEFT, bytes([s])))


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None
        self.rooms = {}        # codigo -> Room
        self.host_room = {}    # addr del anfitrion -> Room
        self.client_room = {}  # addr del cliente -> Room
        self.last_create = {}  # ip -> instante
        self.packets = 0
        # Emulacion de red para pruebas de latencia (todo apagado por defecto)
        self.sim_owd = 0.0       # ms de ida de cada tramo cliente<->rele
        self.sim_host = 0.0      # ms de ida del tramo anfitrion<->rele
        self.sim_jitter = 0.0    # ms de jitter maximo por tramo (uniforme)
        self.sim_loss = 0.0      # % de paquetes perdidos por tramo
        self.sim_extra = {}      # id de cliente -> ms extra de ida
        self.leg = {}            # direccion -> ms de ida de su tramo
        self.rtt = {}            # direccion -> ultimas 60 muestras de ping (ms)
        self.last_ping = 0.0
        self.force_mode = None   # PRUEBAS: fuerza el modo de latencia

    # ---------- util ----------
    def connection_made(self, transport):
        self.transport = transport
        if sys.platform == "win32":
            # En Windows, enviar a un cliente que ya cerro su puerto hace fallar el siguiente recvfrom con
            # WSAECONNRESET y se pierden paquetes de OTROS jugadores. Se desactiva (SIO_UDP_CONNRESET).
            try:
                import ctypes
                from ctypes import wintypes
                ws2 = ctypes.WinDLL("ws2_32")
                ws2.WSAIoctl.argtypes = [ctypes.c_size_t, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                         ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_void_p]
                off, ret = wintypes.DWORD(0), wintypes.DWORD(0)
                sock = transport.get_extra_info("socket")
                ws2.WSAIoctl(sock.fileno(), 0x9800000C, ctypes.byref(off), 4, None, 0, ctypes.byref(ret), None, None)
            except Exception as exc:
                log.warning("no se pudo desactivar SIO_UDP_CONNRESET: %s", exc)

    def error_received(self, exc):
        pass   # errores ICMP de un cliente que se fue: se ignoran

    def send(self, addr, data, src=None):
        if not (self.sim_owd or self.sim_host or self.sim_jitter or self.sim_loss or self.sim_extra):
            self.transport.sendto(data, addr)
            return
        # el paquete cruza el tramo de origen y el de destino (si lo origina el propio rele, cuenta ida y vuelta)
        legs = [self.leg.get(addr, self.sim_owd)]
        legs.append(self.leg.get(src, self.sim_owd) if src is not None else legs[0])
        delay = 0.0
        for ms in legs:
            if self.sim_loss and random.random() * 100.0 < self.sim_loss:
                return
            delay += ms + (random.random() * self.sim_jitter if self.sim_jitter else 0.0)
        if delay <= 0:
            self.transport.sendto(data, addr)
        else:
            asyncio.get_running_loop().call_later(delay / 1000.0, self.transport.sendto, data, addr)

    def send_leg(self, addr, data):
        """Envio de un paquete que solo cruza el tramo rele -> jugador (con emulacion opcional)."""
        if self.sim_loss and random.random() * 100.0 < self.sim_loss:
            return
        delay = self.leg.get(addr, self.sim_owd) + (random.random() * self.sim_jitter if self.sim_jitter else 0.0)
        if delay <= 0:
            self.transport.sendto(data, addr)
        else:
            asyncio.get_running_loop().call_later(delay / 1000.0, self.transport.sendto, data, addr)

    def ingress(self, addr, fn, *args):
        """Entrega un paquete que llega del jugador, retrasado por su tramo si hay emulacion."""
        if self.sim_loss and random.random() * 100.0 < self.sim_loss:
            return
        delay = self.leg.get(addr, self.sim_owd) + (random.random() * self.sim_jitter if self.sim_jitter else 0.0)
        if delay <= 0:
            fn(*args)
        else:
            asyncio.get_running_loop().call_later(delay / 1000.0, fn, *args)

    # ---------- agregador ----------
    def rtt_of(self, addr):
        """(ping de referencia, variacion) en ms, con las ultimas ~60 muestras. Se usa el percentil 90 (no el promedio):
        el retraso de un jugador tiene que cubrir casi todos sus picos, asi un jugador con mala conexion paga
        con su propio retraso y no frena a los demas."""
        r = self.rtt.get(addr)
        if not r:
            return (100.0, 20.0)                # sin ninguna medicion todavia
        v = sorted(r)
        n = len(v)
        if n < 5:
            return (v[-1], max(3.0, v[-1] * 0.1))   # pocas muestras: se usa la peor (conservador)
        ref = v[min(n - 1, (9 * n) // 10)]
        spread = ref - v[n // 2]                # cuanto se aleja el pico del valor tipico
        return (ref, max(3.0, min(spread * 0.5, 150.0)))

    def compute_delays(self, humans, mode=1):
        k, c = LAT_MODES.get(mode, LAT_MODES[1])
        if self.force_mode is not None:
            k, c = LAT_MODES[self.force_mode]
        # El retraso de cada jugador depende SOLO de su propio ping (su mando va al servidor y el fotograma vuelve):
        # un jugador cercano no tiene por que pagar la distancia del mas lejano.
        out = {}
        for s, a in humans.items():
            rtt, dev = self.rtt_of(a)
            need = rtt + k * dev + c
            out[s] = max(MIN_DELAY, min(MAX_DELAY, int(-(-need // TICK_MS))))
        return out

    def on_agg_begin(self, addr, body, now):
        room = self.host_room.get(addr)
        if room is None or len(body) < 1:
            return
        if room.agg is not None and now - room.agg.begin < 5.0:
            return      # duplicado del aviso de inicio (el anfitrion lo repite hasta recibir los parametros)
        n = body[0]
        humans = {0: room.host}
        for k in range(n):
            if 1 + 3 * k + 3 > len(body):
                return
            slot = body[1 + 3 * k]
            (cid,) = struct.unpack_from("<H", body, 2 + 3 * k)
            c = room.clients.get(cid)
            if c is None or slot == 0:
                continue
            humans[slot] = c[0]
        mode = body[1 + 3 * n] if len(body) > 1 + 3 * n else 1
        delays = self.compute_delays(humans, mode)
        room.agg = Agg(self, humans, delays, now)
        mask = 0
        for s in humans:
            mask |= 1 << s
        log.info("agregador: carrera de %d humanos en la sala %s (modo %d), retrasos %s", len(humans), room.code, mode,
                 {s: delays[s] for s in sorted(delays)})
        log.info("agregador: RTT/desviacion medidos (ms) %s",
                 {s: (round(self.rtt_of(a)[0]), round(self.rtt_of(a)[1], 1)) for s, a in sorted(humans.items())})
        for s, a in humans.items():
            pkt = pack(R_AGG_PARAMS, struct.pack("<BH", delays[s], mask))
            room.agg.params[s] = pkt
            room.agg.params_t[s] = now
            self.send(a, pkt)

    def on_agg_input(self, addr, body):
        room = self.host_room.get(addr) or self.client_room.get(addr)
        if room is None or room.agg is None:
            return
        room.agg.on_input(self, addr, body, time.monotonic())

    def on_pong(self, addr, body, now):
        if len(body) < 8:
            return
        (token,) = struct.unpack_from("<d", body, 0)
        sample = (now - token) * 1000.0
        if sample < 0 or sample > 5000:
            return
        r = self.rtt.get(addr)
        if r is None:
            r = self.rtt[addr] = collections.deque(maxlen=60)
        r.append(sample)

    def agg_tick(self):
        now = time.monotonic()
        if now - self.last_ping >= 1.0:
            self.last_ping = now
            tok = struct.pack("<d", now)
            for room in self.rooms.values():
                self.send(room.host, pack(R_PING, tok))
                for _cid, (caddr, _seen) in room.clients.items():
                    self.send(caddr, pack(R_PING, tok))
        for room in self.rooms.values():
            if room.agg is not None:
                room.agg.on_timer(self, now)
                if room.agg.released and now - room.agg.last_emit > 30.0:
                    log.info("agregador: carrera inactiva en %s, se cierra", room.code)
                    room.agg = None

    def new_code(self):
        # solo para pruebas: la primera sala usa un codigo fijo si se indico --test-code
        if getattr(self, "test_code", None) and self.test_code not in self.rooms:
            return self.test_code
        for _ in range(50):
            code = "".join(random.choice(ALPHABET) for _ in range(CODE_LEN))
            if code not in self.rooms:
                return code
        return None

    def drop_room(self, room, notify=True):
        if notify:
            for cid, (addr, _) in room.clients.items():
                self.send(addr, pack(R_ROOM_CLOSED))
                self.client_room.pop(addr, None)
        else:
            for cid, (addr, _) in room.clients.items():
                self.client_room.pop(addr, None)
        self.rooms.pop(room.code, None)
        self.host_room.pop(room.host, None)
        log.info("sala %s cerrada (%d clientes)", room.code, len(room.clients))

    # ---------- paquetes ----------
    def datagram_received(self, data, addr):
        if len(data) < 5 or len(data) > MAX_PAYLOAD + 16:
            return
        magic, ptype = struct.unpack_from("<IB", data, 0)
        if magic != MAGIC:
            return
        body = data[5:]
        self.packets += 1
        now = time.monotonic()

        if ptype == R_CREATE:
            self.on_create(addr, now)
        elif ptype == R_JOIN:
            self.on_join(addr, body, now)
        elif ptype == R_DATA_UP:
            room = self.client_room.get(addr)
            if room and len(body) <= MAX_PAYLOAD:
                cid = room.by_addr.get(addr)
                if cid is not None:
                    room.clients[cid][1] = now
                    self.send(room.host, pack(R_DATA_FROM, struct.pack("<H", cid) + body), src=addr)
        elif ptype == R_DATA_TO:
            room = self.host_room.get(addr)
            if room and len(body) >= 2 and len(body) - 2 <= MAX_PAYLOAD:
                room.host_seen = now
                (cid,) = struct.unpack_from("<H", body, 0)
                c = room.clients.get(cid)
                if c:
                    self.send(c[0], pack(R_DATA_DOWN, body[2:]), src=addr)
        elif ptype == R_KEEPALIVE:
            room = self.host_room.get(addr)
            if room:
                room.host_seen = now
                self.send(addr, pack(R_KEEPALIVE))
            else:
                room = self.client_room.get(addr)
                cid = room.by_addr.get(addr) if room else None
                if cid is not None:
                    room.clients[cid][1] = now
                    self.send(addr, pack(R_KEEPALIVE))
                else:
                    # el servidor no conoce esta sesion (por ejemplo, se reinicio)
                    self.send(addr, pack(R_ERROR, pstr("sesion desconocida")))
        elif ptype == R_PONG:
            self.on_pong(addr, body, now)
        elif ptype == R_AGG_BEGIN:
            self.on_agg_begin(addr, body, now)
        elif ptype == R_AGG_INPUT:
            self.ingress(addr, self.on_agg_input, addr, body)
        elif ptype == R_AGG_LEAVE:
            room = self.host_room.get(addr) or self.client_room.get(addr)
            if room is not None and room.agg is not None:
                s = room.agg.slot_of.get(addr)
                if s is not None:
                    room.agg.drop(self, s)
        elif ptype == R_AGG_END:
            room = self.host_room.get(addr)
            if room is not None and room.agg is not None:
                a = room.agg
                log.info("agregador: fin de carrera en %s (%d ticks, %d rellenos por plazo %s)", room.code, a.n_emit,
                         a.n_fill, {s: n for s, n in a.fill_by_slot.items() if n})
                room.agg = None
        elif ptype == R_CLOSE:
            room = self.host_room.get(addr)
            if room:
                self.drop_room(room)
            else:
                room = self.client_room.pop(addr, None)
                if room:
                    cid = room.by_addr.pop(addr, None)
                    room.clients.pop(cid, None)

    def on_create(self, addr, now):
        room = self.host_room.get(addr)
        if room:  # reintento: mismo codigo
            room.host_seen = now
            self.send(addr, pack(R_CREATED, pstr(room.code)))
            return
        ip = addr[0]
        if now - self.last_create.get(ip, 0) < CREATE_COOLDOWN:
            return
        if len(self.rooms) >= MAX_ROOMS:
            self.send(addr, pack(R_ERROR, pstr("servidor lleno")))
            return
        if sum(1 for r in self.rooms.values() if r.host[0] == ip) >= MAX_ROOMS_PER_IP:
            self.send(addr, pack(R_ERROR, pstr("demasiadas salas desde tu IP")))
            return
        code = self.new_code()
        if code is None:
            self.send(addr, pack(R_ERROR, pstr("sin codigos libres")))
            return
        self.last_create[ip] = now
        room = Room(code, addr)
        self.rooms[code] = room
        self.host_room[addr] = room
        self.leg[addr] = self.sim_host
        log.info("sala %s creada por %s:%d", code, addr[0], addr[1])
        self.send(addr, pack(R_CREATED, pstr(code)))

    def on_join(self, addr, body, now):
        code, _ = read_str(body, 0)
        code = (code or "").strip().upper()
        room = self.rooms.get(code)
        if room is None:
            self.send(addr, pack(R_JOINED, bytes([0]) + pstr("codigo no encontrado")))
            return
        existing = room.by_addr.get(addr)
        if existing is not None:  # reintento
            room.clients[existing][1] = now
            self.send(addr, pack(R_JOINED, bytes([1]) + pstr("ok")))
            return
        if len(room.clients) >= MAX_CLIENTS_PER_ROOM:
            self.send(addr, pack(R_JOINED, bytes([0]) + pstr("sala llena")))
            return
        cid = room.next_id
        room.next_id += 1
        room.clients[cid] = [addr, now]
        room.by_addr[addr] = cid
        self.leg[addr] = self.sim_owd + self.sim_extra.get(cid, 0.0)
        self.client_room[addr] = room
        log.info("cliente %d entro a la sala %s (%s:%d)", cid, code, addr[0], addr[1])
        self.send(addr, pack(R_JOINED, bytes([1]) + pstr("ok")))

    # ---------- limpieza ----------
    def sweep(self):
        now = time.monotonic()
        for room in list(self.rooms.values()):
            if now - room.host_seen > ROOM_TIMEOUT:
                log.info("sala %s: el anfitrion no responde", room.code)
                self.drop_room(room)
                continue
            for cid, (addr, seen) in list(room.clients.items()):
                if now - seen > CLIENT_TIMEOUT:
                    room.clients.pop(cid, None)
                    room.by_addr.pop(addr, None)
                    self.client_room.pop(addr, None)
                    log.info("sala %s: cliente %d sin respuesta", room.code, cid)
        for ip in [i for i, t in self.last_create.items() if now - t > 60]:
            self.last_create.pop(ip, None)


async def main():
    ap = argparse.ArgumentParser(description="Rele de Spaghetti Kart Online")
    ap.add_argument("--port", type=int, default=7778)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--test-code", default=None, help="codigo fijo para la primera sala (solo pruebas)")
    ap.add_argument("--sim-owd", type=float, default=0.0, help="PRUEBAS: ms de ida de cada tramo cliente<->rele")
    ap.add_argument("--sim-host", type=float, default=0.0, help="PRUEBAS: ms de ida del tramo anfitrion<->rele")
    ap.add_argument("--sim-jitter", type=float, default=0.0, help="PRUEBAS: jitter maximo (ms) por tramo")
    ap.add_argument("--sim-loss", type=float, default=0.0, help="PRUEBAS: porcentaje de paquetes perdidos por tramo")
    ap.add_argument("--agg-mode", type=int, default=None, help="PRUEBAS: fuerza el modo de latencia (0 estable, 1 equilibrada, 2 rapida)")
    ap.add_argument("--sim-extra", default="", help="PRUEBAS: ms extra por cliente, ej. 1=40,3=90")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    if sys.platform == "win32":
        try:  # temporizadores de 1 ms (por defecto asyncio redondea a ~15 ms; solo importa en pruebas locales)
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
        except Exception:
            pass
    loop = asyncio.get_running_loop()
    relay = Relay()
    relay.test_code = args.test_code.upper() if args.test_code else None
    relay.sim_owd = args.sim_owd
    relay.force_mode = args.agg_mode
    relay.sim_host = args.sim_host
    relay.sim_jitter = args.sim_jitter
    relay.sim_loss = args.sim_loss
    for item in [x for x in args.sim_extra.split(",") if "=" in x]:
        k, v = item.split("=", 1)
        relay.sim_extra[int(k)] = float(v)
    if relay.sim_owd or relay.sim_host or relay.sim_jitter or relay.sim_loss or relay.sim_extra:
        log.warning("EMULACION DE RED ACTIVA (solo pruebas): owd=%sms host=%sms jitter=%sms perdida=%s%%",
                    relay.sim_owd, relay.sim_host, relay.sim_jitter, relay.sim_loss)
    await loop.create_datagram_endpoint(lambda: relay, local_addr=(args.host, args.port))
    log.info("rele escuchando en UDP %s:%d", args.host, args.port)
    last_sweep = 0.0
    while True:
        await asyncio.sleep(0.01)
        relay.agg_tick()
        now = time.monotonic()
        if now - last_sweep >= 2.0:
            last_sweep = now
            relay.sweep()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
