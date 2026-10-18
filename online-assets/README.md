# Assets del modo online

`spaghetti-places16/` es un mod en carpeta: copiarlo a `mods/` junto al ejecutable.
Contiene los carteles de puesto 9th..16th y la imagen del boton ONLINE del menu.

Se generan con los scripts de `tools/online/` (usan el mk64.o2r ya extraido como fuente;
las rutas del principio de cada script apuntan al ejecutable compilado y hay que ajustarlas):

- `make_places.py write <carpeta>`: carteles 9th..16th (I4 128x64, mismo estilo que 1st..8th).
- `make_online_btn.py`: boton ONLINE (RGBA16 64x18) -> `texture_online`.
