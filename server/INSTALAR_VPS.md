# Servidor de rele en un VPS

El rele solo reenvia paquetes UDP entre anfitrion y clientes; consume muy poco (1 vCPU / 512 MB sobra).
Sirve cualquier VPS de ~4 USD/mes con Ubuntu 22.04/24.04 y IP publica (Hetzner, DigitalOcean, Vultr, Contabo...).

## 1. Subir el servidor

Desde tu PC (cambia `usuario` e `IP`):

```
scp server/relay_server.py usuario@IP:/home/usuario/
```

## 2. Abrir el puerto UDP 7778

```
sudo ufw allow 7778/udp
sudo ufw allow OpenSSH
sudo ufw enable
```

Si el proveedor tiene firewall propio (panel web), abre tambien UDP 7778 ahi.

## 3. Dejarlo corriendo como servicio

Crear `/etc/systemd/system/sk-relay.service` (cambia `usuario`):

```
[Unit]
Description=Spaghetti Kart Online - rele
After=network.target

[Service]
User=usuario
ExecStart=/usr/bin/python3 /home/usuario/relay_server.py --port 7778
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

Activarlo:

```
sudo systemctl daemon-reload
sudo systemctl enable --now sk-relay
sudo systemctl status sk-relay
journalctl -u sk-relay -f      # ver salas y clientes en vivo
```

Necesita Python 3.8+ (`python3 --version`); no usa paquetes externos.

## 4. Comprobar desde tu PC

En la ventana Online del juego: Servidor = IP del VPS, puerto 7778, "Crear sala".
Debe salir un codigo de 5 letras. Si da error de tiempo agotado, el puerto UDP no esta abierto.

## 5. Repartir el juego

```
powershell -File tools/online/empaquetar.ps1 -Relay IP_DEL_VPS
```

Genera `dist/SpaghettiKart-Online.zip` con el servidor ya escrito en la configuracion.
Cada jugador descomprime y ejecuta `Spaghettify.exe`.
