param([string]$Root = (Join-Path $PSScriptRoot '../..'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-tools.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$fixture = @'
<div align="center">
# ![Occlude3d](https://readme-typing-svg.demolab.com/?lines=Occlude3d)
[![Download](https://custom-icon-badges.demolab.com/badge/download?logo=data%3Abase64%2Ctest)](https://example.com/releases)
</div>
## ![Features](https://readme-typing-svg.demolab.com/?lines=Features)
<details open>
<summary><strong>Cutscenes</strong> &mdash; Keep this summary.</summary>
<dl><dd>
Keep **all** feature details &amp; [links](https://example.com/a_(b)).
</dd></dl></details>
<details>
<summary>Closed feature</summary>
Keep closed details, too.
</details>
| `/occlude3d limit <rate>` | `/ashita/<Name>_<id>/` |
Literal `<details>` and ``<summary>`example`</summary>``.
````html
<details><summary>Unchanged code</summary></details>
```
````
    <details>Indented code</details>
~~~html
<div>Also unchanged</div>
~~~
'@
$plain = ConvertTo-ReleaseReadme $fixture
$hardBreak = "First line.  `nSecond line.`n"
Assert-True ((ConvertTo-ReleaseReadme $hardBreak) -ceq $hardBreak) 'Markdown hard line breaks changed.'
foreach ($expected in @(
    '# Occlude3d', '## Features', '### Cutscenes', 'Keep this summary.',
    '[Download](https://example.com/releases)',
    'Keep **all** feature details & [links](https://example.com/a_(b)).',
    '### Closed feature', 'Keep closed details, too.',
    '| `/occlude3d limit <rate>` | `/ashita/<Name>_<id>/` |',
    'Literal `<details>` and ``<summary>`example`</summary>``.',
    '<details><summary>Unchanged code</summary></details>',
    '    <details>Indented code</details>', '<div>Also unchanged</div>'
)) { Assert-True ($plain.Contains($expected)) "Conversion lost: $expected" }
Assert-True ($plain -notmatch 'demolab|base64') 'Decorative image URLs remain.'
Assert-True ((ConvertTo-ReleaseReadme $plain) -ceq $plain) 'Plain Markdown changed on a second conversion.'
$rejected = $false
try { ConvertTo-ReleaseReadme '<iframe src="example"></iframe>' | Out-Null } catch { $rejected = $true }
Assert-True $rejected 'Unsupported HTML should fail instead of silently losing content.'

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('occlude3d-release-test-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $cfg = Get-ReleaseConfig -Root $Root
    $originalPath = Join-Path $Root 'README.md'
    $originalHash = (Get-FileHash -LiteralPath $originalPath).Hash
    $original = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $originalPath).Path)
    foreach ($doc in $cfg.docs) {
        $target = Join-Path $scratch $doc
        [IO.Directory]::CreateDirectory((Split-Path $target -Parent)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Root $doc) -Destination $target
    }
    $extraFiles = @()
    foreach ($extra in $cfg.extras) {
        $path = Join-Path $Root $extra
        $files = if (Test-Path -LiteralPath $path -PathType Container) {
            @(Get-ChildItem -LiteralPath $path -Recurse -File)
        } else { @(Get-Item -LiteralPath $path) }
        $rootPath = (Resolve-Path -LiteralPath $Root).Path
        foreach ($file in $files) {
            $rel = $file.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
            if (($rel -split '/') | Where-Object { $_.StartsWith('.') }) { continue }
            $dest = Join-Path $scratch $rel
            [IO.Directory]::CreateDirectory((Split-Path $dest -Parent)) | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $dest
            $extraFiles += $rel
        }
    }
    foreach ($junk in @('DISCORD_POST.md', 'Screenshot.png', 'tools/local.ps1', 'addons/magiccircle/local.lua.bak')) {
        if ($junk -like 'addons/*' -and @($cfg.extras | Where-Object { Test-Path -LiteralPath (Join-Path $Root $_) -PathType Container }).Count -gt 0) { continue }
        $dest = Join-Path $scratch $junk
        [IO.Directory]::CreateDirectory((Split-Path $dest -Parent)) | Out-Null
        [IO.File]::WriteAllText($dest, 'not for release')
    }
    $dll = Join-Path $scratch 'test.dll'
    [IO.File]::WriteAllBytes($dll, [byte[]]@(0x4D, 0x5A, 0, 1))
    $outDir = Join-Path $scratch 'out'
    foreach ($revision in 1, 2) {
        $source = $original
        if ($revision -eq 2) { $source += "`nREADME edited after building the DLL.`n" }
        [IO.File]::WriteAllText((Join-Path $scratch 'README.md'), $source)
        $zipName = New-ReleasePackage $cfg -Dll $dll -Version '1.0' -Interface '4.30' -BuildInfo @{ test = $true } -OutDir $outDir -Root $scratch
        $zipPath = Join-Path $outDir $zipName
        $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            Assert-True ($zip.Entries.Count -eq ($cfg.docs.Count + $extraFiles.Count + 2)) 'Unexpected files in ZIP.'
            foreach ($doc in $cfg.docs) {
                $entry = $zip.GetEntry("docs/$($cfg.docsFolder)/$(Split-Path $doc -Leaf)")
                Assert-True ($null -ne $entry) "Missing packaged document: $doc"
                $reader = New-Object IO.StreamReader($entry.Open())
                try { $actual = $reader.ReadToEnd() } finally { $reader.Dispose() }
                $expected = [IO.File]::ReadAllText((Join-Path $scratch $doc))
                if ((Split-Path $doc -Leaf) -ieq 'README.md') { $expected = ConvertTo-ReleaseReadme $expected }
                Assert-True ($actual -ceq $expected) "Packaged $doc differs from its expected content."
            }
            foreach ($rel in $extraFiles) {
                $entry = $zip.GetEntry($rel)
                Assert-True ($null -ne $entry) "Missing example file: $rel"
                $memory = New-Object IO.MemoryStream
                $stream = $entry.Open()
                try { $stream.CopyTo($memory); $actual = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
                $path = Join-Path $scratch $rel
                if ((Split-Path $rel -Leaf) -ieq 'README.md') {
                    Assert-True ([Text.Encoding]::UTF8.GetString($actual) -ceq (ConvertTo-ReleaseReadme ([IO.File]::ReadAllText($path)))) 'Example README differs.'
                } else {
                    Assert-True ([Convert]::ToBase64String($actual) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($path))) "Example file changed: $rel"
                }
            }
            $stream = $zip.GetEntry("plugins/$($cfg.dll)").Open()
            try {
                $bytes = New-Object IO.MemoryStream
                $stream.CopyTo($bytes)
                Assert-True ([BitConverter]::ToString($bytes.ToArray()) -eq '4D-5A-00-01') 'DLL changed during packaging.'
            } finally { $stream.Dispose(); $bytes.Dispose() }
        } finally { $zip.Dispose() }
        $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-True ((Get-Content -LiteralPath (Join-Path $outDir 'SHA256SUMS.txt')).Trim() -eq "$hash  $zipName") 'ZIP checksum is incorrect.'
        Assert-True ([IO.File]::ReadAllText((Join-Path $scratch 'README.md')) -ceq $source) 'Packaging overwrote its source README.'
    }
    foreach ($rel in $cfg.extras) {
        if (Test-Path -LiteralPath (Join-Path $scratch $rel) -PathType Container) { continue }
        Remove-Item -LiteralPath (Join-Path $scratch $rel)
        $rejected = $false
        try { New-ReleasePackage $cfg -Dll $dll -Version '1.0' -Interface '4.30' -BuildInfo @{ test = $true } -OutDir $outDir -Root $scratch | Out-Null } catch { $rejected = $true }
        Assert-True $rejected "Missing example file did not stop packaging: $rel"
        Copy-Item -LiteralPath (Join-Path $Root $rel) -Destination (Join-Path $scratch $rel)
    }
    Assert-True ((Get-FileHash -LiteralPath $originalPath).Hash -eq $originalHash) 'Source checkout README was changed.'
    Write-Host 'PASS: README conversion, code preservation, ZIP contents, checksums, and documentation edits after build.'
} finally { Remove-Item -LiteralPath $scratch -Recurse -Force }
