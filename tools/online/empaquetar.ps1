param(
    [string]$Relay = "",          # IP o dominio del servidor de rele (se guarda como valor por defecto en el juego)
    [int]$RelayPort = 7778,
    [string]$Out = "$PSScriptRoot\..\..\dist\SpaghettiKart-Online.zip"
)
# Empaqueta el juego compilado para repartirlo entre varias PCs.
# NO incluye: simbolos de depuracion (.pdb), registros, partidas guardadas ni la configuracion de mandos de este PC.
# OJO: mk64.o2r sale de tu ROM; usalo solo en tus propias PCs, no lo publiques.
$ErrorActionPreference = "Stop"
$src = Resolve-Path "$PSScriptRoot\..\..\build\x64\Release"
$stage = Join-Path $env:TEMP "sk-online-stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

$exclude = @("*.pdb", "*.exp", "*.lib", "*.log", "*.sav", "lakitu_dbg.txt", "online_overlay.txt", "imgui.ini", "spaghettify.cfg.json")
Get-ChildItem $src -Force | Where-Object { $_.Name -ne "logs" } | ForEach-Object {
    $skip = $false
    foreach ($e in $exclude) { if ($_.Name -like $e) { $skip = $true } }
    if (-not $skip) { Copy-Item $_.FullName -Destination $stage -Recurse -Force }
}
# quita basura dentro de las carpetas copiadas
Get-ChildItem $stage -Recurse -Include *.log, *.pdb | Remove-Item -Force -ErrorAction SilentlyContinue

# configuracion minima: solo los datos de la sala online (el resto toma los valores por defecto del juego)
$cfg = [ordered]@{ CVars = [ordered]@{ gOnline = [ordered]@{ Relay = $Relay; RelayPort = $RelayPort; Name = "Jugador"; Mode = 0 } } }
$cfg | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $stage "spaghettify.cfg.json") -Encoding UTF8

$outFull = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path (Split-Path $outFull) | Out-Null
if (Test-Path $outFull) { Remove-Item $outFull -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $outFull -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
"Listo: $outFull ($([math]::Round((Get-Item $outFull).Length / 1MB)) MB)"
if ($Relay -eq "") { "Aviso: no se indico -Relay; cada jugador tendra que escribir el servidor en la ventana Online." }
