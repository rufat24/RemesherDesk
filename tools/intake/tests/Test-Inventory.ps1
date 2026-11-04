<#
  Self-test for tools/intake/Inventory.ps1. Builds a synthetic Unity-like project in a temp
  folder, runs the inventory, and asserts on its outputs. Runs on Windows PowerShell 5.1 and
  PowerShell 7 (Windows or Linux). Exit code 0 = pass.

  -NativeDll: a native PE DLL with exports to plant in the fake project. Defaults to
  %windir%\System32\version.dll on Windows. On Linux, build one with mingw, e.g.
    printf '__declspec(dllexport) int qr_remesh(void){return 0;}\n' > q.c
    x86_64-w64-mingw32-gcc -shared -o q.dll q.c
  If no DLL is available the binary assertions are skipped (reported, not failed).
#>
param([string]$NativeDll = '')
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$inventory = Join-Path (Split-Path -Parent $here) 'Inventory.ps1'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('rdesk_inv_' + [Guid]::NewGuid().ToString('N'))
$proj = Join-Path $tmp 'FakeProject'
$out = Join-Path $tmp 'out'
$fail = New-Object System.Collections.Generic.List[string]
function Check([bool]$cond, [string]$msg) { if (-not $cond) { $script:fail.Add($msg) } }
function MkFile([string]$rel, [string]$text) {
    $p = Join-Path $proj $rel
    $d = Split-Path -Parent $p
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
    [System.IO.File]::WriteAllText($p, $text, (New-Object System.Text.UTF8Encoding($false)))
}
$sep = [System.IO.Path]::DirectorySeparatorChar
function ConvertTo-NativePath([string]$p) { return $p.Replace('/', $sep) }

if ($NativeDll -eq '' -and $env:windir) { $NativeDll = Join-Path $env:windir 'System32\version.dll' }

try {
    MkFile (ConvertTo-NativePath 'Assets/QR/Native~/src/remesh.cpp') 'int x;'
    MkFile (ConvertTo-NativePath 'Assets/QR/Native~/CMakeLists.txt') 'project(qr)'
    MkFile (ConvertTo-NativePath 'Assets/QR/LICENSE.md') 'MIT License'
    MkFile (ConvertTo-NativePath ('Assets/' + [char]0x00DC + 'nicode ' + [char]0x0259 + '/x.cpp')) 'int y;'
    MkFile (ConvertTo-NativePath 'Assets/QR/Scripts/Native.cs') ("using Unity.Burst;`nstatic class N {`n const string Lib = `"QR`";`n [DllImport(Lib)]`n private static extern int qr_remesh(float[] v, int n);`n [DllImport(`"QR`")] static extern int qr_version();`n}`n")
    MkFile (ConvertTo-NativePath 'Library/PackageCache/com.unity.burst@1.8.0/B.cs') '[DllImport("burst")] static extern void x();'
    MkFile (ConvertTo-NativePath 'Temp/junk.cpp') 'ignored'
    MkFile (ConvertTo-NativePath 'ProjectSettings/ProjectVersion.txt') 'm_EditorVersion: 2022.3.10f1'
    MkFile (ConvertTo-NativePath 'Packages/manifest.json') '{"dependencies":{}}'
    $haveDll = ($NativeDll -ne '') -and (Test-Path -LiteralPath $NativeDll)
    if ($haveDll) {
        New-Item -ItemType Directory -Path (Join-Path $proj (ConvertTo-NativePath 'Assets/Plugins/x86_64')) -Force | Out-Null
        Copy-Item -LiteralPath $NativeDll -Destination (Join-Path $proj (ConvertTo-NativePath 'Assets/Plugins/x86_64/QR.dll'))
    }

    & $inventory -ProjectPath $proj -OutDir $out | Out-Null

    $summary = [System.IO.File]::ReadAllText((Join-Path $out 'summary.txt'))
    Check ($summary -match 'native source files \(excl\. com\.unity\.\*\) : 2') 'expected 2 native source files (Temp/ and com.unity.* excluded)'
    Check ($summary -match 'build files\s+: 1') 'expected 1 build file'
    Check ($summary -match 'DllImport=2 extern=2 Burst=1') 'C# interop counts (static extern must be found; com.unity.* skipped)'
    Check ($summary -match 'Lib \(constant') 'DllImport with a constant library name'
    Check ($summary -match '"QR" x1') 'DllImport literal library name'
    $files = [System.IO.File]::ReadAllText((Join-Path $out 'files.csv'))
    Check ($files.Contains([string][char]0x0259)) 'non-ASCII path preserved in files.csv (UTF-8)'
    Check (-not $files.Contains('junk.cpp')) 'Temp/ must be skipped'
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $out 'files.csv'))
    Check (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB)) 'no UTF-8 BOM'
    Check (Test-Path -LiteralPath (Join-Path $out (ConvertTo-NativePath 'meta/ProjectSettings__ProjectVersion.txt'))) 'ProjectVersion.txt copied'
    Check (Test-Path -LiteralPath ($out + '.zip')) 'zip written'
    if ($haveDll) {
        Check ($summary -match 'native binaries\s+: 1') 'one native binary'
        Check ($summary -match 'PE32\+ x64, exports=[1-9]') 'PE32+ x64 with exports parsed'
        $exp = [System.IO.File]::ReadAllText((Join-Path $out 'exports.txt'))
        Check ($exp -match '^== ') 'exports.txt lists the DLL'
    } else {
        Write-Host 'SKIP: no native DLL available; binary assertions skipped'
    }
} finally {
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    if (Test-Path -LiteralPath ($out + '.zip')) { Remove-Item -LiteralPath ($out + '.zip') -Force }
}
foreach ($f in $fail) { Write-Host ('FAIL: ' + $f) }
Write-Host ('Test-Inventory: ' + $(if ($fail.Count -eq 0) { 'ok' } else { [string]$fail.Count + ' failure(s)' }) + ' (PowerShell ' + $PSVersionTable.PSVersion + ')')
if ($fail.Count -ne 0) { exit 1 } else { exit 0 }
