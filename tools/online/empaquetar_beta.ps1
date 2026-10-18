param(
    [string]$Version = "beta1",
    [string]$Relay = $env:SK_RELAY_HOST,  # servidor de rele que se guarda por defecto en el juego (o define SK_RELAY_HOST)
    [int]$RelayPort = 7778,
    [string]$Discord = "",                # enlace de invitacion al Discord (si se deja vacio, no se menciona)
    [string]$Out = ""
)
# Paquete BETA para repartir a desconocidos: SOLO el programa y el mod, SIN la ROM ni ningun archivo de Nintendo.
# Se niega a terminar si encuentra mk64.o2r, una ROM (.z64/.n64/.v64) o algo parecido dentro del paquete.
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$src = Join-Path $root "build\x64\Release"
if (-not (Test-Path "$src\Spaghettify.exe")) { throw "No existe $src\Spaghettify.exe: compila primero." }
if ([string]::IsNullOrWhiteSpace($Relay)) { throw "Falta el servidor de rele: usa -Relay <host> o define la variable de entorno SK_RELAY_HOST." }
if ($Out -eq "") { $Out = Join-Path $root "dist\SpaghettiKart-Online-$Version.zip" }
$stage = Join-Path $env:TEMP "sk-beta-stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# Lo que NO va: simbolos de depuracion, registros, partidas, configuracion de esta PC, archivos de prueba y, sobre todo,
# todo lo que sale de la ROM de Nintendo (mk64.o2r y las ROMs).
$exclude = @("*.pdb", "*.exp", "*.lib", "*.log", "*.sav", "imgui.ini", "lakitu_dbg.txt", "online_overlay.txt", "net_dbg.txt",
             "online_end_dbg.txt", "spaghettify.cfg.json", "mk64.o2r", "*.z64", "*.n64", "*.v64", "*.rom", "logs")
Get-ChildItem $src -Force | ForEach-Object {
    $skip = $false
    foreach ($e in $exclude) { if ($_.Name -like $e) { $skip = $true } }
    if (-not $skip) { Copy-Item $_.FullName -Destination $stage -Recurse -Force }
}
foreach ($e in $exclude) { Get-ChildItem $stage -Recurse -Force -Include $e -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }

# Configuracion minima: solo la sala online (el resto, ajustes graficos incluidos, toma los valores por defecto del juego)
$cfg = [ordered]@{ CVars = [ordered]@{ gOnline = [ordered]@{ Relay = $Relay; RelayPort = $RelayPort; Name = "Jugador"; Mode = 0 } } }
$cfg | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $stage "spaghettify.cfg.json") -Encoding UTF8

$exeHash = (Get-FileHash (Join-Path $stage "Spaghettify.exe") -Algorithm SHA256).Hash
$discEs = ""; $discEn = ""
if ($Discord -ne "") { $discEs = "Comunidad y reportes de errores (Discord): $Discord"; $discEn = "Community and bug reports (Discord): $Discord" }

@"
SPAGHETTI KART ONLINE  -  $Version
==================================================================================
Proyecto personal NO OFICIAL. Version BETA: puede tener errores.
Unofficial personal project. BETA version: expect bugs.
$discEs
$discEn
==================================================================================

----------------------------------------------------------------------------------
ESPANOL
----------------------------------------------------------------------------------
QUE ES
Una version de SpaghettiKart (el port para PC de Mario Kart 64, de Harbour Masters) con juego EN LINEA:
salas con codigo, hasta 16 jugadores por sala, lista de salas y un modo de items extra (Modo Chaos).

ESTE PAQUETE NO INCLUYE NINGUN ARCHIVO DE NINTENDO
Necesitas tu propia copia de Mario Kart 64: version US, formato .z64
(SHA-1: 579C48E211AE952530FFC8738709F078D5DD215E). No distribuimos ni ayudamos a conseguir ROMs.

INSTALAR
1. ANTES de extraer: clic derecho al .zip > Propiedades > marca "Desbloquear" > Aceptar.
   (Asi Windows no marca los archivos como descargados de internet y casi siempre desaparece el aviso.)
2. Extrae TODO el contenido en una carpeta tuya (por ejemplo, Documentos\SpaghettiKartOnline).
3. Abre Spaghettify.exe. La primera vez te pedira tu ROM US y generara el archivo mk64.o2r en tu PC.
4. En la pantalla GAME SELECT pulsa Z (teclado: la tecla Z) para abrir el menu ONLINE.
   Escribe tu nombre, crea una sala o unete con el codigo (o desde la lista de salas).
   Cambia el idioma en la fila IDIOMA / LANGUAGE del menu online.
Teclado por defecto: Shift = A, Ctrl = B, Enter = START, Z = Z. Tambien funciona con mando.

AVISO DE WINDOWS (SmartScreen)
El programa NO esta firmado digitalmente (un certificado cuesta dinero), asi que Windows puede decir "Windows protegio tu PC".
No es un virus. Si quieres comprobarlo: pulsa "Mas informacion" > "Ejecutar de todas formas".
Para verificar que es el archivo original, el SHA-256 de Spaghettify.exe de este paquete es:
   $exeHash

PRIVACIDAD (que ve el servidor)
Para jugar en linea el juego se conecta a un servidor de salas. El servidor ve tu direccion IP y los controles que
mandas durante las carreras (los necesita para que todos vean lo mismo), y guarda un registro de las salas y de
estadisticas de conexion (ping, retrasos). NO recoge archivos, ni datos de tu PC, ni tu ROM.

CONSEJOS PARA JUGAR MEJOR
- Lo que mas importa no es la velocidad de internet, sino que sea ESTABLE: mejor por cable que por Wi-Fi, y cierra descargas.
- En la sala veras el ping de cada jugador (verde = bien, amarillo = regular, rojo = alto o inestable).
- Si tu PC va justa, activa MODO LIGERO en la sala (resolucion 1x, sin suavizado, 30 FPS).
- El servidor es pequeno: una sala de hasta 16 jugadores a la vez. En la beta habra pocas salas.

PROBLEMAS
Si algo falla, cuentanos que paso y a que hora (con la hora podemos revisar el servidor). El archivo logs\Spaghetti Kart.log
de tu carpeta puede ayudar.

----------------------------------------------------------------------------------
ENGLISH
----------------------------------------------------------------------------------
WHAT IT IS
A version of SpaghettiKart (Harbour Masters' PC port of Mario Kart 64) with ONLINE play:
rooms with a code, up to 16 players per room, a room list and an extra items mode (Chaos Mode).

THIS PACKAGE INCLUDES NO NINTENDO FILES
You need your own copy of Mario Kart 64: US version, .z64 format
(SHA-1: 579C48E211AE952530FFC8738709F078D5DD215E). We do not distribute or help you get ROMs.

INSTALL
1. BEFORE extracting: right-click the .zip > Properties > tick "Unblock" > OK.
   (This stops Windows from marking the files as downloaded from the internet, which usually removes the warning.)
2. Extract EVERYTHING into a folder of your own (for example, Documents\SpaghettiKartOnline).
3. Run Spaghettify.exe. The first time it will ask for your US ROM and generate mk64.o2r on your PC.
4. On the GAME SELECT screen press Z (keyboard: the Z key) to open the ONLINE menu.
   Enter your name, create a room or join with a code (or from the room list).
   Change the language on the IDIOMA / LANGUAGE row of the online menu.
Default keyboard: Shift = A, Ctrl = B, Enter = START, Z = Z. Controllers work too.

WINDOWS WARNING (SmartScreen)
The program is NOT digitally signed (a certificate costs money), so Windows may say "Windows protected your PC".
It is not a virus. If you want to check: click "More info" > "Run anyway".
To verify the original file, the SHA-256 of Spaghettify.exe in this package is:
   $exeHash

PRIVACY (what the server sees)
To play online the game connects to a room server. The server sees your IP address and the controller inputs you
send during races (it needs them so everyone sees the same thing), and keeps a log of rooms and connection
statistics (ping, delays). It does NOT collect files, data from your PC, or your ROM.

TIPS FOR BETTER PLAY
- What matters most is not internet speed but STABILITY: a cable is better than Wi-Fi, and close downloads.
- In the room you will see each player's ping (green = good, yellow = fair, red = high or unstable).
- If your PC struggles, turn on LITE MODE in the room (1x resolution, no anti-aliasing, 30 FPS).
- The server is small: one room of up to 16 players at a time. The beta will have few rooms.

PROBLEMS
If something fails, tell us what happened and at what time (with the time we can check the server). The file
logs\Spaghetti Kart.log in your folder may help.
"@ | Set-Content (Join-Path $stage "LEEME-README.txt") -Encoding UTF8

@"
Spaghetti Kart Online ($Version) / CREDITOS - CREDITS
-----------------------------------------------------
Mod online (salas por servidor, hasta 16 jugadores, lockstep) / Online mod (server rooms, up to 16 players, lockstep): ezracorp
Juego base / Base game: SpaghettiKart, by Harbour Masters - https://github.com/HarbourMasters/SpaghettiKart
Motor y herramientas / Engine and tools: libultraship (MIT License, kenix3) and Torch (MIT License, Lywx), included in SpaghettiKart.

Proyecto de aficionados, NO oficial: no esta afiliado ni respaldado por Nintendo ni por Harbour Masters.
Fan project, UNOFFICIAL: not affiliated with or endorsed by Nintendo or Harbour Masters.
Mario Kart es marca de Nintendo / Mario Kart is a trademark of Nintendo.
"@ | Set-Content (Join-Path $stage "CREDITOS-CREDITS.txt") -Encoding UTF8

# --- Revision de seguridad: nada de Nintendo ni de esta PC dentro del paquete ---
$bad = Get-ChildItem $stage -Recurse -Force -File | Where-Object { $_.Name -match '^mk64\.o2r$|\.(z64|n64|v64|rom|sav|pdb|log)$' -or $_.Name -in @('imgui.ini') }
if ($bad) { Remove-Item $stage -Recurse -Force; throw ("Archivos prohibidos en el paquete: " + (($bad | ForEach-Object { $_.Name }) -join ", ")) }
$big = Get-ChildItem $stage -Recurse -Force -File | Where-Object { $_.Length -gt 40MB }
if ($big) { Remove-Item $stage -Recurse -Force; throw ("Archivo sospechosamente grande (la ROM u otro): " + (($big | ForEach-Object { $_.Name + " " + [math]::Round($_.Length / 1MB) + " MB" }) -join ", ")) }

$outFull = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path (Split-Path $outFull) | Out-Null
if (Test-Path $outFull) { Remove-Item $outFull -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $outFull -CompressionLevel Optimal
"--- contenido del paquete ---"
Get-ChildItem $stage -Force | ForEach-Object { "{0,-28} {1,10:N0} bytes" -f $_.Name, $(if ($_.PSIsContainer) { (Get-ChildItem $_.FullName -Recurse -File | Measure-Object Length -Sum).Sum } else { $_.Length }) }
Remove-Item $stage -Recurse -Force
$zipHash = (Get-FileHash $outFull -Algorithm SHA256).Hash
"--- listo ---"
"Zip:        $outFull  ($([math]::Round((Get-Item $outFull).Length / 1MB, 1)) MB)"
"SHA-256 zip: $zipHash"
"SHA-256 exe: $exeHash"
Set-Content ([System.IO.Path]::ChangeExtension($outFull, ".sha256.txt")) @("zip  $zipHash  $(Split-Path $outFull -Leaf)", "exe  $exeHash  Spaghettify.exe") -Encoding ASCII
