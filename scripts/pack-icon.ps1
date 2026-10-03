param([Parameter(Mandatory)][string]$SourcePng)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$iconRoot = Join-Path $projectRoot 'config/icons'
New-Item -ItemType Directory -Force -Path $iconRoot | Out-Null
Copy-Item -LiteralPath $SourcePng -Destination (Join-Path $iconRoot 'cod4x1khz-blue.png') -Force
Add-Type -AssemblyName System.Drawing
$source = [System.Drawing.Bitmap]::new($SourcePng)
try {
    $images = @()
    foreach ($size in @(16,32,48,256)) {
        $bitmap = [System.Drawing.Bitmap]::new($size,$size,[System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
            $graphics.DrawImage($source,[System.Drawing.Rectangle]::new(0,0,$size,$size),0,0,$source.Width,$source.Height,[System.Drawing.GraphicsUnit]::Pixel)
            $stream = [System.IO.MemoryStream]::new()
            try {
                $bitmap.Save($stream,[System.Drawing.Imaging.ImageFormat]::Png)
                $images += [pscustomobject]@{ Size=$size; Data=$stream.ToArray() }
                if ($size -eq 32) { $bitmap.Save((Join-Path $iconRoot 'cod4x1khz-blue-32.png'),[System.Drawing.Imaging.ImageFormat]::Png) }
            } finally { $stream.Dispose() }
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
    $output = [System.IO.File]::Create((Join-Path $iconRoot 'cod4x1khz.ico'))
    $writer = [System.IO.BinaryWriter]::new($output)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$images.Count)
        $offset = 6 + 16*$images.Count
        foreach ($image in $images) {
            $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$image.Data.Length); $writer.Write([uint32]$offset)
            $offset += $image.Data.Length
        }
        foreach ($image in $images) { $writer.Write([byte[]]$image.Data) }
    } finally { $writer.Dispose() }
    $transparentPixels = 0
    for ($y=0;$y -lt $source.Height;$y++) { for ($x=0;$x -lt $source.Width;$x++) { if ($source.GetPixel($x,$y).A -eq 0) { $transparentPixels++ } } }
    [pscustomobject]@{ SourceWidth=$source.Width; SourceHeight=$source.Height; FullyTransparentPixels=$transparentPixels; Icon=(Join-Path $iconRoot 'cod4x1khz.ico') }
} finally { $source.Dispose() }
