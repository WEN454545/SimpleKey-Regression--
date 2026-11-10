Add-Type -AssemblyName System.Drawing
# ???? SimpleKey ??????1024x1024?????????¦Å? + ?????????????
$size = 1024
$bmp = New-Object System.Drawing.Bitmap($size, $size)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.Clear([System.Drawing.Color]::Transparent)

# ?????¦Á??????????
$bgBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 51, 102, 204))
$rect = New-Object System.Drawing.Rectangle(32, 32, 960, 960)
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$r = 180
$path.AddArc($rect.X, $rect.Y, $r, $r, 180, 90)
$path.AddArc($rect.Right - $r, $rect.Y, $r, $r, 270, 90)
$path.AddArc($rect.Right - $r, $rect.Bottom - $r, $r, $r, 0, 90)
$path.AddArc($rect.X, $rect.Bottom - $r, $r, $r, 90, 90)
$path.CloseFigure()
$g.FillPath($bgBrush, $path)

# ??????????4x5 ????????
$keyBrush = [System.Drawing.Brushes]::White
$keySize = 110
$gapX = 190; $gapY = 150
$offsetX = 135; $offsetY = 180
for ($row = 0; $row -lt 5; $row++) {
    for ($col = 0; $col -lt 4; $col++) {
        $x = $offsetX + $col * $gapX
        $y = $offsetY + $row * $gapY
        $g.FillRectangle($keyBrush, $x, $y, $keySize, $keySize)
    }
}

$bmp.Save("d:\???\???????\host-app\app-icon.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "icon saved"
