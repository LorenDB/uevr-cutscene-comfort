# Grab the Halo window client area and report two numbers about it.
#
# The point of the test is that a viewpoint translation should move the scene
# inside the aperture while leaving the mask itself exactly where it is. So we
# measure those two regions separately: a patch in the middle of the left eye
# for scene motion, and a band along the frame edge for mask motion.

param([Parameter(Mandatory)][string]$OutPath, [string]$Title = 'Halo: Campaign Evolved')

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Grab {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr Find(string t) {
    IntPtr f = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      if(!IsWindowVisible(h)) return true;
      StringBuilder sb = new StringBuilder(300); GetWindowText(h,sb,300);
      if (sb.ToString().Contains(t)) { f=h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
}
"@

$h = [Grab]::Find($Title)
if ($h -eq [IntPtr]::Zero) { 'window not found'; exit 1 }

$r = New-Object Grab+RECT
[void][Grab]::GetClientRect($h, [ref]$r)
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
if ($w -le 0 -or $ht -le 0) { 'bad rect'; exit 1 }

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][Grab]::PrintWindow($h, $hdc, 3)
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $OutPath ${w}x${ht}"
