function Get-ProjectSourceRevision {
    param([Parameter(Mandatory)][string]$ProjectRoot, [Parameter(Mandatory)][string]$RelativePath)
    $checkoutPath = Join-Path $ProjectRoot $RelativePath
    $sourceTop = & git -C $checkoutPath rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE) { throw "Source checkout is not versioned: $checkoutPath" }
    $sourceTop = [IO.Path]::GetFullPath($sourceTop.Trim())
    $root = [IO.Path]::GetFullPath($ProjectRoot)
    if ($sourceTop.TrimEnd('\','/') -eq $root.TrimEnd('\','/')) {
        # GitHub clones contain the complete edited source, not nested Git links.
        $vendor = Get-Content -LiteralPath (Join-Path $root 'config/vendor-snapshots.json') -Raw | ConvertFrom-Json
        $entry = $vendor.sources | Where-Object path -EQ $RelativePath
        if (!$entry) { throw "Missing vendor provenance: $RelativePath" }
        $sentinel = "$RelativePath/$($entry.sentinel)"
        & git -C $root ls-files --error-unmatch -- $sentinel 1>$null 2>$null
        if ($LASTEXITCODE -or !(Test-Path -LiteralPath (Join-Path $root $sentinel))) {
            throw "Incomplete tracked source snapshot: $RelativePath"
        }
        return $entry.base_commit
    }
    return (& git -C $checkoutPath rev-parse HEAD).Trim()
}
