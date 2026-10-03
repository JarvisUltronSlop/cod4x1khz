# Dot-sourced by benchmark.ps1; every hash runs before launch or after cleanup.
if (!('Cod4x1khzBenchmarkOwnedImage' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class Cod4x1khzBenchmarkOwnedImage {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref uint size);
    public static string Read(IntPtr ownedHandle) {
        uint size = 32768;
        var path = new StringBuilder((int)size);
        if (!QueryFullProcessImageNameW(ownedHandle, 0, path, ref size))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return path.ToString();
    }
}
'@
}
function Get-KHZFileDigest([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Get-KHZBenchmarkSnapshot([string]$ProjectRoot, [string]$ArchivePath) {
    $root = [IO.Path]::GetFullPath($ProjectRoot)
    $files = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    $archiveMismatches = @()
    foreach ($relative in @('cod4x1khz.exe', 'runtime/cod4x1khz.exe', 'runtime/cod4x1khz_server.exe', 'runtime/cod4x1khz_bench.exe',
            'runtime/mss32.dll', 'runtime/D3DX9_43.dll', 'runtime/binkw32.dll', 'runtime/steam_api.dll',
            'benchmark.ps1', 'launch-client.ps1', 'launch-server.ps1', 'launch-bench.ps1', 'upstream-lock.json',
            'scripts/benchmark_provenance.ps1', 'scripts/monitor.py', 'scripts/wait_ready.py', 'scripts/reset_server_stats.py',
            'runtime/client.cfg', 'runtime/server.cfg')) { [void]$files.Add($relative) }
    # These are the stock-map runtime asset containers; profiles and generated
    # outputs are deliberately excluded because a running game updates them.
    foreach ($directory in @('runtime/main', 'runtime/zone')) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $root $directory) -File -Recurse -ErrorAction Stop) {
            if ($file.Extension -in @('.iwd', '.ff')) {
                [void]$files.Add([IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/'))
            }
        }
    }
    if (!$ArchivePath) {
        $candidates = @(Get-ChildItem -LiteralPath (Join-Path $root 'results') -Filter 'build*-source-and-binary-hashes.json' -File |
            Where-Object { $_.Name -match '^build([0-9]+)-source-and-binary-hashes\.json$' } |
            Sort-Object { [int]([regex]::Match($_.Name, '^build([0-9]+)').Groups[1].Value) } -Descending)
        if (!$candidates.Count) { throw 'A successful archived build is required for a benchmark' }
        $ArchivePath = $candidates[0].FullName
    }
    $archiveFull = [IO.Path]::GetFullPath($ArchivePath)
    $archiveRelative = [IO.Path]::GetRelativePath($root, $archiveFull).Replace('\', '/')
    if ($archiveRelative -notmatch '^results/build[0-9]+-source-and-binary-hashes\.json$') { throw 'Build archive is outside the expected project result path' }
    [void]$files.Add($archiveRelative)
    $hashes = [ordered]@{}
    foreach ($relative in $files) { $hashes[$relative] = Get-KHZFileDigest (Join-Path $root $relative) }
    $archive = Get-Content -LiteralPath $archiveFull -Raw | ConvertFrom-Json -Depth 20
    if (!$archive.tests.passed -or $archive.tests.native_suites -le 0 -or !$archive.source_sha256) { throw 'Build archive lacks successful native suites or its source inventory' }
    foreach ($relative in @('cod4x1khz.exe', 'runtime/cod4x1khz.exe', 'runtime/cod4x1khz_server.exe', 'runtime/cod4x1khz_bench.exe',
            'runtime/mss32.dll', 'runtime/D3DX9_43.dll', 'runtime/binkw32.dll', 'runtime/steam_api.dll')) {
        if ($archive.binary_sha256.$relative -cne $hashes[$relative]) { $archiveMismatches += $relative }
    }
    foreach ($property in $archive.runtime_asset_packages.PSObject.Properties) {
        if (!$hashes.Contains($property.Name) -or $hashes[$property.Name] -cne $property.Value) { $archiveMismatches += $property.Name }
    }
    [pscustomobject]@{
        recorded_utc = [DateTime]::UtcNow.ToString('o')
        sha256 = $hashes
        archive_matches = $archiveMismatches.Count -eq 0
        archive_mismatches = $archiveMismatches
        source_archive = [ordered]@{
            path = $archiveRelative; sha256 = $hashes[$archiveRelative]; build = $archive.build
            native_suites = $archive.tests.native_suites; source_files = @($archive.source_sha256.PSObject.Properties).Count
            source_pins = $archive.source_pins; inventory_note = $archive.source_inventory_note
        }
        scope = 'SHA256 of actual root/runtime game binaries, startup runtime DLLs, all runtime main IWD/zone FF containers, benchmark tooling/configuration and the immutable source/build archive. Generated profiles are excluded. Archive source hashes are provenance, not a claim that every staged source was compiled.'
    }
}

function Compare-KHZBenchmarkSnapshots($Before, $After) {
    $differences = @()
    foreach ($key in @($Before.sha256.Keys) + @($After.sha256.Keys) | Sort-Object -Unique) {
        if (!$Before.sha256.Contains($key) -or !$After.sha256.Contains($key) -or $Before.sha256[$key] -cne $After.sha256[$key]) { $differences += $key }
    }
    [pscustomobject]@{ passed = $differences.Count -eq 0; changed_files = $differences }
}

function Get-KHZOwnedProcessEvidence($Process, [string]$ExpectedPath, [string]$Role) {
    # Start-Process may return before the PE loader publishes its module path.
    # Keep the exact owned handle and require the actual image; never substitute
    # the expected filename for missing native process metadata.
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    $loadedPath = $null
    $lastError = $null
    do {
        $Process.Refresh()
        if ($Process.HasExited) { throw "Owned $Role exited before the benchmark began" }
        # Query the process's primary image using the retained handle. During
        # WOW64 startup, MainModule enumeration may return ntdll.dll instead.
        try { $loadedPath = [Cod4x1khzBenchmarkOwnedImage]::Read($Process.Handle) }
        catch { $lastError = $_.Exception.Message }
        if (![string]::IsNullOrWhiteSpace($loadedPath)) { break }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $deadline)
    if ([string]::IsNullOrWhiteSpace($loadedPath)) { throw "Owned $Role image path was not available within 5 seconds: $lastError" }
    $actual = [IO.Path]::GetFullPath($loadedPath)
    $expected = [IO.Path]::GetFullPath($ExpectedPath)
    if (![StringComparer]::OrdinalIgnoreCase.Equals($actual, $expected)) { throw "Owned $Role loaded an unexpected executable: $actual" }
    [pscustomobject]@{ role = $Role; pid = $Process.Id; started_utc = $Process.StartTime.ToUniversalTime().ToString('o'); image = $actual; expected_image = $expected; image_verified = $true }
}
