# Drives kit/build/FableCoop.bat against a fake Steam folder and a fake game (the real exe copied in).
# usage: powershell -NoProfile -File kit\test_kit.ps1 <fixture dir>
#   fixture: steam\ (libraryfolders.vdf, config\loginusers.vdf), lib2\ (appmanifest + game), docs\ (fake Documents)
param([string]$Fix)
$ErrorActionPreference = 'Stop'
$bat = Join-Path $PSScriptRoot 'build\FableCoop.bat'
$game = Join-Path $Fix 'lib2\steamapps\common\Fable Anniversary'
$bin = Join-Path $game 'Binaries\Win32'
$env:COOP_STEAM_ROOT = Join-Path $Fix 'steam'
$env:FC_DOCUMENTS = Join-Path $Fix 'docs'
$env:FC_NO_LAUNCH = '1'
$fails = 0
function Run([string]$a) { $out = cmd /c "`"$bat`" $a 2>&1" | Out-String; return @{ code = $LASTEXITCODE; out = $out } }
function Check([string]$name, [bool]$ok, $r) { if ($ok) { "PASS $name" } else { "FAIL $name`n$($r.out)"; $script:fails++ } }
function Sha($p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower() }
$oldDll = Sha "$bin\dinput8.dll"

$r = Run 'install'
Check 'install finds the game through libraryfolders.vdf' ($r.code -eq 0 -and $r.out -match [regex]::Escape($game)) $r
Check 'install backs up a foreign dinput8.dll' ((Test-Path "$bin\dinput8.dll.fablecoop-backup") -and (Sha "$bin\dinput8.dll.fablecoop-backup") -eq $oldDll) $r
Check 'install writes steam_appid.txt' ((Get-Content "$bin\steam_appid.txt" -Raw) -eq '288470') $r
$r = Run 'install'
Check 'second install: already up to date' ($r.code -eq 0 -and $r.out -match 'deja installe') $r

$r = Run 'play guest 123'
Check 'play refuses a bad SteamID' ($r.code -ne 0 -and $r.out -match 'invalide') $r
$r = Run 'play guest 76561190000000002'
Check 'play refuses my own SteamID (loginusers MostRecent)' ($r.code -ne 0 -and $r.out -match 'propre identifiant') $r
$r = Run 'play host 76561190000000003'
Check 'play ok, shows my id' ($r.code -eq 0 -and $r.out -match '76561190000000002') $r
Check 'play copies the saves' ((Get-Content "$game\CoopProfiles\coop\My Games\FableHD\Saves\Fable Anniversary" -Raw).Trim() -eq 'save') $r
Check 'play remembers the peer' ((Get-Content "$game\CoopProfiles\coop\peer.txt" -Raw).Trim() -eq '76561190000000003') $r

$r = Run "uninstall --game-dir `"$game`""
Check 'uninstall restores the old dinput8.dll' ($r.code -eq 0 -and (Sha "$bin\dinput8.dll") -eq $oldDll -and -not (Test-Path "$bin\dinput8.dll.fablecoop-backup")) $r
Check 'uninstall removes steam_appid.txt it created' (-not (Test-Path "$bin\steam_appid.txt")) $r

Copy-Item "$bin\Fable Anniversary.exe" "$bin\exe.keep"
[IO.File]::WriteAllBytes("$bin\Fable Anniversary.exe", [byte[]](1, 2, 3))
$r = Run 'install'
Check 'wrong game build refused, nothing written' ($r.code -ne 0 -and $r.out -match 'integrite' -and (Sha "$bin\dinput8.dll") -eq $oldDll -and -not (Test-Path "$bin\steam_appid.txt")) $r
Move-Item -Force "$bin\exe.keep" "$bin\Fable Anniversary.exe"

Remove-Item Env:COOP_STEAM_ROOT
$r = Run "install --game-dir `"$Fix\nowhere`""
Check 'missing game: plain message' ($r.code -ne 0 -and $r.out -match 'introuvable') $r

"$fails failed"
exit $fails
