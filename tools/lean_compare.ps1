# Compare two captures taken before and after a viewpoint translation.
#
# Two regions, two very different expectations:
#
#   scene   a patch inside the left eye aperture. A viewpoint translation
#           should change this, and by more than sampling noise.
#   mask    a thin band hugging the outer edge, which is solid black mask in
#           both shots. The mask is drawn in screen space so it must not move,
#           and this should stay at essentially zero.

param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B
)

Add-Type -AssemblyName System.Drawing

$ia = [System.Drawing.Bitmap]::FromFile($A)
$ib = [System.Drawing.Bitmap]::FromFile($B)

if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) { 'size mismatch'; exit 1 }

$w = $ia.Width; $h = $ia.Height
$half = [int]($w / 2)

function Get-MeanAbsDiff($x0, $y0, $x1, $y1, $step) {
    $sum = 0.0; $n = 0
    for ($y = $y0; $y -lt $y1; $y += $step) {
        for ($x = $x0; $x -lt $x1; $x += $step) {
            $pa = $ia.GetPixel($x, $y); $pb = $ib.GetPixel($x, $y)
            $sum += [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)
            $n++
        }
    }
    if ($n -eq 0) { return @(0.0, 0) }
    return @([Math]::Round($sum / ($n * 3), 3), $n)
}

# Inside the left eye aperture, kept to the outer part of that half. UEVR's
# own menu panel sits across the middle of the window when it is open, and
# that panel is static, so sampling over it only dilutes the signal.
$sx0 = [int]($half * 0.08); $sx1 = [int]($half * 0.40)
$sy0 = [int]($h * 0.35);    $sy1 = [int]($h * 0.65)
$scene = Get-MeanAbsDiff $sx0 $sy0 $sx1 $sy1 5

# Outer edge band, solid mask in both shots.
$mask = Get-MeanAbsDiff 2 2 ($w - 2) ([int]($h * 0.05)) 6

"scene region  meanAbsDiff = $($scene[0])   samples=$($scene[1])"
"mask  region  meanAbsDiff = $($mask[0])   samples=$($mask[1])"

$ia.Dispose(); $ib.Dispose()
