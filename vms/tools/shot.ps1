# 실행 중인 gstream_VMS 창을 캡처한다.
param([string]$Out = "shot.png", [int]$ClickX = -1, [int]$ClickY = -1)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
}
"@

$p = Get-Process gstream_VMS -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no process"; exit 1 }
$h = $p.MainWindowHandle
[void][W]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400

$r = New-Object W+R
[void][W]::GetWindowRect($h, [ref]$r)

# 클릭 좌표가 주어지면 창 기준 상대 좌표로 클릭한다
if ($ClickX -ge 0) {
  [void][W]::SetCursorPos($r.L + $ClickX, $r.T + $ClickY)
  Start-Sleep -Milliseconds 150
  [W]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
  [W]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
  Start-Sleep -Milliseconds 700
}

$w = $r.Rt - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
"saved $Out ($w x $ht)"
