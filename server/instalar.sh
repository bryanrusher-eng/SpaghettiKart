#!/usr/bin/env bash
# Instala el rele de Spaghetti Kart Online como servicio (Ubuntu/Debian). Ejecutar como root:
#   sudo bash instalar.sh
# Antes hay que haber subido relay_server.py a la misma carpeta que este archivo.
set -euo pipefail

PORT="${1:-7778}"
DIR="/opt/sk-relay"
SRC="$(cd "$(dirname "$0")" && pwd)/relay_server.py"

if [ "$(id -u)" -ne 0 ]; then echo "Ejecutalo con sudo."; exit 1; fi
if [ ! -f "$SRC" ]; then echo "No encuentro relay_server.py junto a este script."; exit 1; fi
command -v python3 >/dev/null || { apt-get update -y && apt-get install -y python3; }

mkdir -p "$DIR"
cp "$SRC" "$DIR/relay_server.py"

cat > /etc/systemd/system/sk-relay.service <<EOF
[Unit]
Description=Spaghetti Kart Online - rele
After=network.target

[Service]
ExecStart=/usr/bin/python3 $DIR/relay_server.py --port $PORT
Restart=always
RestartSec=2
DynamicUser=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now sk-relay

# firewall del servidor (si ufw existe); el del panel del proveedor hay que abrirlo a mano
if command -v ufw >/dev/null; then
    ufw allow OpenSSH >/dev/null || true
    ufw allow "$PORT"/udp >/dev/null
    ufw --force enable >/dev/null
fi

# Imagenes de Oracle Cloud: traen reglas iptables que bloquean todo menos SSH; hay que insertar la nuestra.
if command -v iptables >/dev/null; then
    iptables -C INPUT -p udp --dport "$PORT" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$PORT" -j ACCEPT
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save >/dev/null 2>&1 || true; fi
fi

sleep 1
systemctl --no-pager --lines=5 status sk-relay || true
echo
echo "Listo. Rele escuchando en UDP $PORT. Recuerda abrir ese puerto UDP tambien en el panel del proveedor."
echo "Ver registro en vivo: journalctl -u sk-relay -f"
