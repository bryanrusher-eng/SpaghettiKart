#!/usr/bin/env python3
"""
Servidor de rele para Spaghetti Kart Online.

Permite jugar por internet sin abrir puertos ni usar VPN: el anfitrion crea una sala
(el servidor le da un codigo corto) y los clientes entran con ese codigo. El servidor solo
reenvia paquetes UDP entre el anfitrion y cada cliente; no interpreta el contenido del juego.

Uso:
    python3 relay_server.py [--port 7778] [--verbose]

Hace falta abrir el puerto UDP elegido en el firewall del servidor.
"""

import argparse
import asyncio
import logging
import random
import struct
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

MAX_PAYLOAD = 1300
MAX_CLIENTS_PER_ROOM = 15
MAX_ROOMS_PER_IP = 3
MAX_ROOMS = 500
ROOM_TIMEOUT = 15.0   # segundos sin noticias del anfitrion
CLIENT_TIMEOUT = 15.0
CREATE_COOLDOWN = 1.0  # segundos minimos entre creaciones desde una misma IP

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

    # ---------- util ----------
    def connection_made(self, transport):
        self.transport = transport

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
    ap.add_argument("--sim-extra", default="", help="PRUEBAS: ms extra por cliente, ej. 1=40,3=90")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    loop = asyncio.get_running_loop()
    relay = Relay()
    relay.test_code = args.test_code.upper() if args.test_code else None
    relay.sim_owd = args.sim_owd
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
    while True:
        await asyncio.sleep(2.0)
        relay.sweep()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
