<#
.SYNOPSIS
  RemesherDesk intake step 0a: inventory a Unity project's native/interop footprint WITHOUT
  copying source code. Produces <OutDir>\ and <OutDir>.zip for you to review, then send.

.DESCRIPTION
  Read-only. Writes only under -OutDir. Works in Windows PowerShell 5.1 and PowerShell 7+.
  Does not need Visual Studio: PE headers and DLL export tables are parsed in-script.
  If dumpbin.exe is on PATH (run from "Developer PowerShell for VS"), its output is added too.

  Outputs (all UTF-8 without BOM):
    summary.txt          human-readable verdict hints - READ THIS FIRST
    files.csv            every interesting file: relative path, category, size, sha256 (binaries)
    binaries.csv         every .dll/.so/.dylib/.bundle/.a/.lib: format, machine, managed?, export count
    exports.txt          export names of native DLLs (parsed from the PE export table)
    interop_cs.csv       every matching line in .cs files: DllImport, extern, Burst, Marshal, ...
    dumpbin.txt          (only if dumpbin is available) /dependents /exports of native DLLs
    meta\...             copies of small metadata files: ProjectVersion.txt, manifest.json,
                         packages-lock.json, LICENSE/COPYING/NOTICE/README, package.json, *.asmdef
    errors.txt           paths that could not be read (e.g. >260 chars, permissions)

  interop_cs.csv contains single source lines (e.g. a DllImport signature). Review before sending.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Inventory.ps1
.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File .\Inventory.ps1 -ProjectPath D:\Unity\Quadremesh -OutDir D:\qr_intake
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'C:\Users\user\UnityProjects\Quadremesh',
    [string]$OutDir = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'qr_intake'),
    [int]$MaxMetaFiles = 300
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Utf8([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}
function Write-Csv([string]$Path, [object[]]$Rows, [string[]]$Columns) {
    # Export-Csv in Windows PowerShell 5.1 defaults to ASCII and turns non-ASCII paths into '?'.
    # ConvertTo-Csv + explicit UTF-8 avoids that on every PowerShell version.
    if ($Rows.Count -eq 0) { Write-Utf8 $Path (($Columns | ForEach-Object { '"' + $_ + '"' }) -join ','); return }
    $lines = $Rows | Select-Object -Property $Columns | ConvertTo-Csv -NoTypeInformation
    Write-Utf8 $Path (($lines -join "`r`n") + "`r`n")
}
function Get-RelPath([string]$Full) {
    if ($Full.StartsWith($script:Root, [StringComparison]::OrdinalIgnoreCase)) {
        return $Full.Substring($script:Root.Length).TrimStart('\', '/')
    }
    return $Full
}

# ---------------------------------------------------------------- PE parsing (no dumpbin needed)
function Get-PeInfo([string]$Path) {
    $r = [ordered]@{ Format = 'unknown'; Machine = ''; Managed = ''; Exports = @(); Note = '' }
    try {
        $fi = New-Object System.IO.FileInfo($Path)
        if ($fi.Length -gt 512MB) { $r.Note = 'too large to parse'; return $r }
        $b = [System.IO.File]::ReadAllBytes($Path)
        if ($b.Length -ge 4 -and $b[0] -eq 0x7F -and $b[1] -eq 0x45 -and $b[2] -eq 0x4C -and $b[3] -eq 0x46) { $r.Format = 'ELF'; return $r }
        if ($b.Length -ge 4 -and (([BitConverter]::ToUInt32($b, 0) -eq 0xFEEDFACF) -or ([BitConverter]::ToUInt32($b, 0) -eq 0xBEBAFECA))) { $r.Format = 'Mach-O'; return $r }
        if ($b.Length -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $r }        # 'MZ'
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        if ($pe -le 0 -or $pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x00004550) { $r.Format = 'MZ (not PE)'; return $r }
        $machine = [BitConverter]::ToUInt16($b, $pe + 4)
        $nSections = [BitConverter]::ToUInt16($b, $pe + 6)
        $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
        $opt = $pe + 24
        $magic = [BitConverter]::ToUInt16($b, $opt)
        $r.Machine = switch ($machine) { 0x8664 { 'x64' } 0x14c { 'x86' } 0xAA64 { 'ARM64' } 0x1c4 { 'ARM' } default { '0x{0:X4}' -f $machine } }
        if ($magic -eq 0x20b) { $r.Format = 'PE32+'; $dd = $opt + 112 } elseif ($magic -eq 0x10b) { $r.Format = 'PE32'; $dd = $opt + 96 } else { $r.Format = 'PE?'; return $r }
        $clrRva = [BitConverter]::ToUInt32($b, $dd + 14 * 8)
        if ($clrRva -ne 0) { $r.Managed = 'managed(.NET)' } else { $r.Managed = 'native' }
        $expRva = [BitConverter]::ToUInt32($b, $dd + 0 * 8)
        if ($expRva -eq 0) { return $r }
        $secTable = $opt + $optSize
        $sections = @()
        for ($i = 0; $i -lt $nSections; $i++) {
            $s = $secTable + 40 * $i
            $sections += , @([BitConverter]::ToUInt32($b, $s + 12), [BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 20), [BitConverter]::ToUInt32($b, $s + 16))
        }   # VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData
        $toOff = {
            param([uint32]$rva)
            foreach ($s in $sections) {
                $size = [Math]::Max([int64]$s[1], [int64]$s[3])
                if ([int64]$rva -ge [int64]$s[0] -and [int64]$rva -lt [int64]$s[0] + $size) { return [int64]$rva - [int64]$s[0] + [int64]$s[2] }
            }
            return [int64]-1
        }
        $eo = & $toOff $expRva
        if ($eo -lt 0) { $r.Note = 'export dir not mapped'; return $r }
        $nNames = [BitConverter]::ToUInt32($b, [int]($eo + 24))
        $namesRva = [BitConverter]::ToUInt32($b, [int]($eo + 32))
        $no = & $toOff $namesRva
        $names = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt [Math]::Min($nNames, 100000); $i++) {
            $nameOff = & $toOff ([BitConverter]::ToUInt32($b, [int]($no + 4 * $i)))
            if ($nameOff -lt 0) { continue }
            $end = [int]$nameOff
            while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
            $names.Add([System.Text.Encoding]::ASCII.GetString($b, [int]$nameOff, $end - [int]$nameOff))
        }
        $r.Exports = $names.ToArray()
    } catch {
        $r.Note = 'parse error: ' + $_.Exception.Message
    }
    return $r
}

# ---------------------------------------------------------------- scan
if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container)) { throw "Project path not found: $ProjectPath" }
$script:Root = (Resolve-Path -LiteralPath $ProjectPath).ProviderPath.TrimEnd('\', '/')
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutDir 'meta') | Out-Null

# Everything at top level except Unity's generated folders; Library\PackageCache is added back
# because embedded/registry packages (possibly the remesher itself) live there.
$skipTop = @('Library', 'Temp', 'Logs', 'obj', 'UserSettings', 'Build', 'Builds', '.git', '.vs', '.idea')
$roots = @(Get-ChildItem -LiteralPath $script:Root -Directory -Force | Where-Object { $skipTop -notcontains $_.Name } | ForEach-Object { $_.FullName })
$pc = Join-Path $script:Root (Join-Path 'Library' 'PackageCache')
if (Test-Path -LiteralPath $pc) { $roots += $pc }

$scanErrors = @()
$all = New-Object System.Collections.Generic.List[System.IO.FileSystemInfo]
foreach ($r in $roots) {
    $err = $null
    # -Force includes hidden/system files (e.g. Native~ folders are not hidden, but some vendors hide things).
    $items = Get-ChildItem -LiteralPath $r -Recurse -Force -ErrorAction SilentlyContinue -ErrorVariable err
    foreach ($i in @($items)) { if ($null -ne $i) { $all.Add($i) } }
    foreach ($e in @($err)) { if ($null -ne $e) { $scanErrors += [string]$e.TargetObject + ' : ' + $e.Exception.Message } }
}
$files = @($all | Where-Object { -not $_.PSIsContainer })
# macOS plugin bundles are directories named *.bundle - record them as binaries.
$bundles = @($all | Where-Object { $_.PSIsContainer -and $_.Name -like '*.bundle' })

$extCategory = @{
    '.c' = 'native-src'; '.cc' = 'native-src'; '.cpp' = 'native-src'; '.cxx' = 'native-src'; '.c++' = 'native-src'
    '.h' = 'native-src'; '.hh' = 'native-src'; '.hpp' = 'native-src'; '.hxx' = 'native-src'; '.inl' = 'native-src'; '.ipp' = 'native-src'
    '.m' = 'native-src'; '.mm' = 'native-src'; '.cu' = 'native-src'; '.cuh' = 'native-src'; '.ispc' = 'native-src'; '.asm' = 'native-src'
    '.dll' = 'binary'; '.so' = 'binary'; '.dylib' = 'binary'; '.a' = 'binary'; '.lib' = 'binary'; '.pdb' = 'debug-symbols'; '.exp' = 'binary'
    '.def' = 'build'; '.vcxproj' = 'build'; '.sln' = 'build'; '.props' = 'build'; '.targets' = 'build'; '.gn' = 'build'; '.bazel' = 'build'; '.mk' = 'build'
    '.compute' = 'gpu-shader'; '.hlsl' = 'gpu-shader'; '.cginc' = 'gpu-shader'
    '.asmdef' = 'unity-asmdef'; '.cs' = 'csharp'
}
$nameCategory = '^(CMakeLists\.txt|.*\.cmake|Makefile|meson\.build|premake.*\.lua|build\.(sh|bat|ps1)|vcpkg\.json|conanfile\.(txt|py))$'
$metaName = '^(LICEN[CS]E|COPYING|NOTICE|README|THIRD[-_ ]?PARTY|Third Party Notices|CHANGELOG)([._-].*)?$|^package\.json$'

$rows = New-Object System.Collections.Generic.List[object]
foreach ($f in $files) {
    $ext = $f.Extension.ToLowerInvariant()
    $cat = $null
    if ($extCategory.ContainsKey($ext)) { $cat = $extCategory[$ext] }
    elseif ($f.Name -match $nameCategory) { $cat = 'build' }
    elseif ($f.Name -match $metaName) { $cat = 'license-or-meta' }
    if ($null -eq $cat) { continue }
    $rel = Get-RelPath $f.FullName
    # Unity's own packages are noise for everything except csharp interop scanning.
    $isUnityPkg = $rel -match '^Library[\\/]+PackageCache[\\/]+com\.unity\.'
    if ($isUnityPkg -and $cat -ne 'binary') { continue }
    $hash = ''
    if ($cat -eq 'binary') { try { $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash } catch { $hash = 'unreadable' } }
    $rows.Add([pscustomobject]@{ RelPath = $rel; Category = $cat; Extension = $ext; Bytes = $f.Length; Sha256 = $hash; UnityPackage = $isUnityPkg })
}
foreach ($d in $bundles) {
    $rows.Add([pscustomobject]@{ RelPath = (Get-RelPath $d.FullName); Category = 'binary'; Extension = '.bundle(dir)'; Bytes = 0; Sha256 = ''; UnityPackage = $false })
}
Write-Csv (Join-Path $OutDir 'files.csv') $rows.ToArray() @('RelPath', 'Category', 'Extension', 'Bytes', 'Sha256', 'UnityPackage')

# ---------------------------------------------------------------- binaries
$binRows = New-Object System.Collections.Generic.List[object]
$exportsText = New-Object System.Text.StringBuilder
foreach ($row in @($rows | Where-Object { $_.Category -eq 'binary' -and -not $_.UnityPackage -and $_.Extension -ne '.bundle(dir)' })) {
    $full = Join-Path $script:Root $row.RelPath
    $pe = Get-PeInfo $full
    $binRows.Add([pscustomobject]@{ RelPath = $row.RelPath; Format = $pe.Format; Machine = $pe.Machine; Kind = $pe.Managed; ExportCount = @($pe.Exports).Count; Bytes = $row.Bytes; Sha256 = $row.Sha256; Note = $pe.Note })
    if ($pe.Managed -eq 'native') {
        [void]$exportsText.AppendLine('== ' + $row.RelPath + ' (' + $pe.Machine + ', ' + @($pe.Exports).Count + ' exports)')
        foreach ($n in $pe.Exports) { [void]$exportsText.AppendLine('   ' + $n) }
    }
}
Write-Csv (Join-Path $OutDir 'binaries.csv') $binRows.ToArray() @('RelPath', 'Format', 'Machine', 'Kind', 'ExportCount', 'Bytes', 'Sha256', 'Note')
Write-Utf8 (Join-Path $OutDir 'exports.txt') $exportsText.ToString()

$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if ($null -ne $dumpbin) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($b in @($binRows | Where-Object { $_.Kind -eq 'native' })) {
        [void]$sb.AppendLine('== ' + $b.RelPath)
        $out = & $dumpbin.Source /nologo /dependents /exports (Join-Path $script:Root $b.RelPath) 2>&1
        foreach ($l in @($out)) { [void]$sb.AppendLine([string]$l) }
    }
    Write-Utf8 (Join-Path $OutDir 'dumpbin.txt') $sb.ToString()
}

# ---------------------------------------------------------------- C# interop scan
# NB: C# is usually written "static extern" (the draft's 'extern static' pattern missed it),
# and Select-String -List would keep only the first hit per file; neither is used here.
$patterns = [ordered]@{
    'DllImport'       = '\[\s*(System\.Runtime\.InteropServices\.)?DllImport\b'
    'extern'          = '\bextern\b'
    'LibraryImport'   = '\[\s*LibraryImport\b'
    'Burst'           = '\[\s*BurstCompile\b|using\s+Unity\.Burst\b'
    'Jobs'            = '\bIJob(ParallelFor)?\b|\bIJobFor\b'
    'Marshal'         = '\bMarshal\.\w+|\[\s*StructLayout\b|\[\s*MarshalAs\b|UnmanagedFunctionPointer|MonoPInvokeCallback'
    'Unsafe'          = '\bunsafe\b|\bfixed\s*\(|NativeArray<|UnsafeUtility\.'
    'ComputeShader'   = '\bComputeShader\b|\bComputeBuffer\b|GraphicsBuffer\b'
    'EditorUI'        = ':\s*(Editor|EditorWindow|ScriptableObject|MonoBehaviour)\b|\[\s*CustomEditor\b'
}
$csRows = New-Object System.Collections.Generic.List[object]
$dllNames = @{}
foreach ($f in @($files | Where-Object { $_.Extension -eq '.cs' })) {
    $rel = Get-RelPath $f.FullName
    if ($rel -match '^Library[\\/]+PackageCache[\\/]+com\.unity\.') { continue }
    $lineNo = 0
    try { $lines = [System.IO.File]::ReadAllLines($f.FullName) } catch { $scanErrors += $rel + ' : ' + $_.Exception.Message; continue }
    foreach ($line in $lines) {
        $lineNo++
        foreach ($k in $patterns.Keys) {
            if ($line -match $patterns[$k]) {
                $t = $line.Trim()
                if ($t.Length -gt 300) { $t = $t.Substring(0, 300) + '...' }
                $csRows.Add([pscustomobject]@{ RelPath = $rel; Line = $lineNo; Kind = $k; Text = $t })
            }
        }
        $m = [regex]::Match($line, 'DllImport\s*\(\s*(?:"(?<lit>[^"]+)"|(?<sym>[A-Za-z_][\w\.]*))')
        if ($m.Success) {
            $key = if ($m.Groups['lit'].Success) { '"' + $m.Groups['lit'].Value + '"' } else { $m.Groups['sym'].Value + ' (constant - see interop_cs.csv)' }
            if ($dllNames.ContainsKey($key)) { $dllNames[$key]++ } else { $dllNames[$key] = 1 }
        }
    }
}
Write-Csv (Join-Path $OutDir 'interop_cs.csv') $csRows.ToArray() @('RelPath', 'Line', 'Kind', 'Text')

# ---------------------------------------------------------------- metadata copies
$metaCount = 0
$metaTargets = @()
foreach ($p in @(@('ProjectSettings', 'ProjectVersion.txt'), @('Packages', 'manifest.json'), @('Packages', 'packages-lock.json'))) {
    $full = Join-Path $script:Root (Join-Path $p[0] $p[1])
    if (Test-Path -LiteralPath $full) { $metaTargets += (Get-Item -LiteralPath $full) }
}
$metaTargets += @($files | Where-Object { ($_.Name -match $metaName -or $_.Extension -eq '.asmdef') -and ((Get-RelPath $_.FullName) -notmatch '^Library[\\/]+PackageCache[\\/]+com\.unity\.') -and $_.Length -lt 1MB })
foreach ($f in $metaTargets) {
    if ($metaCount -ge $MaxMetaFiles) { break }
    $rel = Get-RelPath $f.FullName
    $dest = Join-Path (Join-Path $OutDir 'meta') ($rel -replace '[\\/:]', '__')
    Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
    $metaCount++
}

# ---------------------------------------------------------------- summary
$count = { param($c) @($rows | Where-Object { $_.Category -eq $c -and -not $_.UnityPackage }).Count }
$nSrc = & $count 'native-src'; $nBuild = & $count 'build'; $nGpu = & $count 'gpu-shader'
$nativeBins = @($binRows | Where-Object { $_.Kind -eq 'native' -or $_.Format -eq 'ELF' -or $_.Format -eq 'Mach-O' })
$managedBins = @($binRows | Where-Object { $_.Kind -eq 'managed(.NET)' })
$kindCount = { param($k) @($csRows | Where-Object { $_.Kind -eq $k }).Count }
$burstDir = Join-Path $script:Root (Join-Path 'Library' 'BurstCache')

$s = New-Object System.Text.StringBuilder
[void]$s.AppendLine('RemesherDesk intake inventory')
[void]$s.AppendLine('project      : ' + $script:Root)
[void]$s.AppendLine('generated    : ' + [DateTime]::UtcNow.ToString('o'))
[void]$s.AppendLine('powershell   : ' + $PSVersionTable.PSVersion.ToString())
[void]$s.AppendLine('dumpbin      : ' + $(if ($null -ne $dumpbin) { $dumpbin.Source } else { 'not found (exports parsed in-script instead)' }))
[void]$s.AppendLine('')
[void]$s.AppendLine('native source files (excl. com.unity.*) : ' + $nSrc)
[void]$s.AppendLine('build files                              : ' + $nBuild)
[void]$s.AppendLine('GPU shader files (.compute/.hlsl/.cginc)  : ' + $nGpu)
[void]$s.AppendLine('native binaries                          : ' + $nativeBins.Count)
foreach ($b in $nativeBins) { [void]$s.AppendLine('    ' + $b.RelPath + '  [' + $b.Format + ' ' + $b.Machine + ', exports=' + $b.ExportCount + ']') }
[void]$s.AppendLine('managed (.NET) DLLs                      : ' + $managedBins.Count)
[void]$s.AppendLine('C# lines: DllImport=' + (& $kindCount 'DllImport') + ' extern=' + (& $kindCount 'extern') + ' Burst=' + (& $kindCount 'Burst') +
    ' Jobs=' + (& $kindCount 'Jobs') + ' Marshal=' + (& $kindCount 'Marshal') + ' ComputeShader=' + (& $kindCount 'ComputeShader'))
[void]$s.AppendLine('DllImport library names:')
foreach ($k in $dllNames.Keys) { [void]$s.AppendLine('    ' + $k + ' x' + $dllNames[$k]) }
[void]$s.AppendLine('Library\BurstCache present: ' + (Test-Path -LiteralPath $burstDir))
[void]$s.AppendLine('metadata files copied: ' + $metaCount + '   scan errors: ' + $scanErrors.Count)
[void]$s.AppendLine('')
[void]$s.AppendLine('Hints (the planning session makes the real call - see docs/intake.md decision tree):')
if ($nSrc -gt 0) { [void]$s.AppendLine('  * Native source present -> likely branch S (build from source).') }
if ($nSrc -eq 0 -and $nativeBins.Count -gt 0) { [void]$s.AppendLine('  * Native binaries but no source -> likely branch D (prebuilt DLL).') }
if ($nSrc -eq 0 -and $nativeBins.Count -eq 0 -and ((& $kindCount 'Burst') -gt 0 -or (& $kindCount 'Jobs') -gt 0)) { [void]$s.AppendLine('  * No native code, Burst/Jobs C# present -> likely branch P (port / host C#).') }
if ($nGpu -gt 0 -and (& $kindCount 'ComputeShader') -gt 0) { [void]$s.AppendLine('  * Compute shaders are used from C#: part of the algorithm may run on the GPU.') }
Write-Utf8 (Join-Path $OutDir 'summary.txt') $s.ToString()
Write-Utf8 (Join-Path $OutDir 'errors.txt') (($scanErrors | ForEach-Object { [string]$_ }) -join "`r`n")

$zip = $OutDir.TrimEnd('\', '/') + '.zip'
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zip
Write-Host $s.ToString()
Write-Host ('Wrote ' + $OutDir + ' and ' + $zip + ' - review the files, then send the zip.')
