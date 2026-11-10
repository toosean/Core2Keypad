#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Core2Keypad 端到端验证：串口注入触摸手势，Windows 全局鼠标钩子确认系统真收到了。

.DESCRIPTION
    不依赖手指，也不需要盯着屏幕猜。流程：
      1. 挂 WH_MOUSE_LL 低级鼠标钩子
      2. 用 C2_T 往固件里注入按下/移动/抬起（和真实触摸走同一条状态机）
      3. 断言：滚轮增量、左右键、移动事件、抽屉开关、工具切换
    跑之前确认设备已连上主机（C2_INFO 里 connected=1）。

    注意：测试会真的移动鼠标、真的点右键 —— 运行期间别把光标停在重要窗口上。
    退出码 0 = 全过；1 = 有失败项。

.EXAMPLE
    pwsh tools/c2_e2e.ps1
    pwsh tools/c2_e2e.ps1 -Port COM3
#>
param(
    [string]$Port = 'COM6',
    [int]$Baud = 921600
)

$ErrorActionPreference = 'Stop'

$hookSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class C2MouseHook
{
    delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x; public int y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT
    {
        public POINT pt;
        public uint mouseData;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MSG
    {
        public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam;
        public uint time; public int ptX; public int ptY;
    }

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);
    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hhk);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG lpMsg, IntPtr hWnd, uint min, uint max, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG lpMsg);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG lpMsg);
    [DllImport("kernel32.dll", CharSet = CharSet.Auto)] static extern IntPtr GetModuleHandle(string name);

    const int WH_MOUSE_LL = 14;
    const uint PM_REMOVE = 1;

    static readonly LowLevelMouseProc _proc = HookCallback;
    static IntPtr _hook = IntPtr.Zero;
    static Thread _thread;
    static volatile bool _running;
    public static readonly List<string> Events = new List<string>();

    static IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            int msg = wParam.ToInt32();
            MSLLHOOKSTRUCT d = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
            string s = null;
            switch (msg)
            {
                case 0x0200: s = "MOVE"; break;
                case 0x0201: s = "LBUTTON_DOWN"; break;
                case 0x0202: s = "LBUTTON_UP"; break;
                case 0x0204: s = "RBUTTON_DOWN"; break;
                case 0x0205: s = "RBUTTON_UP"; break;
                case 0x020A: s = "WHEEL " + (short)((d.mouseData >> 16) & 0xFFFF); break;
            }
            if (s != null) lock (Events) Events.Add(s);
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    public static bool Start()
    {
        _running = true;
        _thread = new Thread(delegate ()
        {
            _hook = SetWindowsHookEx(WH_MOUSE_LL, _proc, GetModuleHandle(null), 0);
            MSG msg;
            while (_running)
            {
                while (PeekMessage(out msg, IntPtr.Zero, 0, 0, PM_REMOVE)) { TranslateMessage(ref msg); DispatchMessage(ref msg); }
                Thread.Sleep(5);
            }
            if (_hook != IntPtr.Zero) UnhookWindowsHookEx(_hook);
        });
        _thread.IsBackground = true;
        _thread.Start();
        Thread.Sleep(300);
        return _hook != IntPtr.Zero;
    }

    public static void Stop() { _running = false; }
}
'@

Add-Type -TypeDefinition $hookSource -Language CSharp
Add-Type -Namespace C2E2E -Name Native -MemberDefinition '
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
'
if (-not [C2MouseHook]::Start()) { throw 'SetWindowsHookEx 失败，挂不上鼠标钩子' }

$link = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$link.DtrEnable = $false
$link.RtsEnable = $false
$link.ReadTimeout = 300
try { $link.Open() } catch { [C2MouseHook]::Stop(); throw "打不开 $Port : $($_.Exception.Message)" }
Start-Sleep -Milliseconds 400
$link.DiscardInBuffer()

function Cmd([string]$c, [int]$wait = 45) {
    $link.Write("$c`n")
    Start-Sleep -Milliseconds $wait
    return $link.ReadExisting()
}
function Info { return ((Cmd 'C2_INFO' 300) -split "`n" | Where-Object { $_ -match 'C2_INFO core2' } | Select-Object -Last 1).Trim() }
function Take { [C2MouseHook]::Events.Clear(); Start-Sleep -Milliseconds 120 }

$results = New-Object System.Collections.Generic.List[object]
function Check([string]$name, [bool]$ok, [string]$detail) {
    $results.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    $mark = if ($ok) { 'PASS' } else { 'FAIL' }
    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("  [{0}] {1}  {2}" -f $mark, $name, $detail) -ForegroundColor $color
}

Write-Host ''
Write-Host ("设备: " + (Info)) -ForegroundColor DarkGray

# --- 1. 上滑 -> 滚轮向上 ---
Cmd 'C2_MODE 0' 150 | Out-Null
Start-Sleep -Milliseconds 200
Take
Cmd 'C2_T 1 160 200' 30 | Out-Null
Cmd 'C2_T 1 160 160' 30 | Out-Null
Cmd 'C2_T 1 160 100' 30 | Out-Null
Cmd 'C2_T 1 160 60' 30  | Out-Null
Cmd 'C2_T 0 160 60' 250 | Out-Null
$ev = @([C2MouseHook]::Events)
$up = @($ev | Where-Object { $_ -eq 'WHEEL -120' }).Count
Check '滑动模式 · 上滑 140px → 滚动向下' ($up -ge 4 -and $up -le 6) "WHEEL -120 x$up（合并容忍 ±1）"

# --- 2. 下滑 -> 滚轮向下 ---
Take
Cmd 'C2_T 1 160 60' 30  | Out-Null
Cmd 'C2_T 1 160 140' 30 | Out-Null
Cmd 'C2_T 1 160 200' 30 | Out-Null
Cmd 'C2_T 0 160 200' 250 | Out-Null
$ev = @([C2MouseHook]::Events)
$down = @($ev | Where-Object { $_ -eq 'WHEEL 120' }).Count
Check '滑动模式 · 下滑 140px → 滚动向上' ($down -ge 4 -and $down -le 6) "WHEEL +120 x$down（合并容忍 ±1）"

# --- 3. 单击 -> 左键 ---
Cmd 'C2_MODE 1' 200 | Out-Null
Take
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$reply = Cmd 'C2_T 1 200 120' 30
$pressMs = $sw.ElapsedMilliseconds
$reply += Cmd 'C2_T 0 200 120' 250
$ev = @([C2MouseHook]::Events)
$cls = if ($reply -match 'right click') { 'right' } elseif ($reply -match 'left click') { 'left' } else { '?' }
Check '鼠标模式 · 单击' (($ev -contains 'LBUTTON_DOWN') -and ($ev -contains 'LBUTTON_UP')) ("设备判为 $cls，按住 ${pressMs}ms；事件=" + ($ev -join ', '))

# --- 4. 长按 -> 右键 ---
Take
Cmd 'C2_T 1 200 120' 700 | Out-Null
Cmd 'C2_T 0 200 120' 300 | Out-Null
$ev = @([C2MouseHook]::Events)
Check '鼠标模式 · 长按 700ms' (($ev -contains 'RBUTTON_DOWN') -and ($ev -contains 'RBUTTON_UP')) ($ev -join ', ')

# --- 5. 拖动 -> 移动 ---
# 先把光标挪到屏幕中间，否则连续测试会把它顶到角落，移动被系统夹住
$sw = [C2E2E.Native]::GetSystemMetrics(0); $sh = [C2E2E.Native]::GetSystemMetrics(1)
[C2E2E.Native]::SetCursorPos([int]($sw / 2), [int]($sh / 2)) | Out-Null
Start-Sleep -Milliseconds 200
$p0 = New-Object C2E2E.Native+POINT
[C2E2E.Native]::GetCursorPos([ref]$p0) | Out-Null
Take
Cmd 'C2_T 1 150 120' 40 | Out-Null
Cmd 'C2_T 1 175 140' 40 | Out-Null
Cmd 'C2_T 1 205 165' 40 | Out-Null
Cmd 'C2_T 0 205 165' 250 | Out-Null
$p1 = New-Object C2E2E.Native+POINT
[C2E2E.Native]::GetCursorPos([ref]$p1) | Out-Null
$dx = $p1.X - $p0.X; $dy = $p1.Y - $p0.Y
$dist = [math]::Sqrt($dx * $dx + $dy * $dy)
Check '鼠标模式 · 拖动' ($dist -ge 20) "光标移动 $([int]$dist)px ($dx,$dy)"

# --- 5b. 按住片刻再拖 -> 左键拖拽 ---
Take
Cmd 'C2_T 1 160 120' 20 | Out-Null
Start-Sleep -Milliseconds 340
Cmd 'C2_T 1 200 150' 50 | Out-Null
Cmd 'C2_T 1 230 170' 50 | Out-Null
Cmd 'C2_T 0 230 170' 250 | Out-Null
$ev = @([C2MouseHook]::Events)
$dragOk = ($ev -contains 'LBUTTON_DOWN') -and ($ev -contains 'MOVE') -and ($ev -contains 'LBUTTON_UP')
Check '按住后拖动 → 左键拖拽' $dragOk ($ev -join ', ')

# --- 5c. 双指上下滑 -> 滚轮 ---
Take
Cmd 'C2_T2 1 120 100 200 100' 90 | Out-Null
Cmd 'C2_T2 1 120 160 200 160' 90 | Out-Null
Cmd 'C2_T2 1 120 220 200 220' 90 | Out-Null
Cmd 'C2_T2 0 120 220 200 220' 250 | Out-Null
$ev = @([C2MouseHook]::Events)
$wheels = @($ev | Where-Object { $_ -eq 'WHEEL 120' }).Count
Check '双指下滑 → 滚动向上' ($wheels -ge 3) "WHEEL +120 x$wheels"

# --- 6. 左缘右滑 -> 抽屉 ---
Take
Cmd 'C2_T 1 5 120' 40 | Out-Null
Cmd 'C2_T 1 40 120' 40 | Out-Null
Cmd 'C2_T 1 95 120' 40 | Out-Null
Cmd 'C2_T 0 95 120' 250 | Out-Null
$info = Info
Check '左缘右滑 · 打开抽屉' ($info -match 'drawer=1') $info
$ev = @([C2MouseHook]::Events)
Check '左缘手势不漏鼠标事件' ($ev.Count -eq 0) ("events=$($ev.Count)")

# --- 7. 点 VIBRATION ---
$before = Info
Cmd 'C2_T 1 60 130' 90  | Out-Null
Cmd 'C2_T 0 60 130' 250 | Out-Null
$after = Info
$b = if ($before -match 'vib=1') { 1 } else { 0 }
$a = if ($after  -match 'vib=1') { 1 } else { 0 }
Check '抽屉 · VIBRATION 切换' ($a -ne $b) "vib $b -> $a"

# --- 8. 点 MODE ---
$before = Info
Cmd 'C2_T 1 170 130' 90  | Out-Null
Cmd 'C2_T 0 170 130' 250 | Out-Null
$after = Info
Check '抽屉 · MODE 切换' (($before -match 'mode=(\w+)') -and ($after -match 'mode=(\w+)') -and ($Matches.Count -gt 0)) "$($before -replace '.*mode=', '') -> $($after -replace '.*mode=', '')"
Check '切模式时抽屉保持打开' ($after -match 'drawer=1') $after

# --- 9. 点抽屉外 -> 收起 ---
Cmd 'C2_T 1 285 120' 90  | Out-Null
Cmd 'C2_T 0 285 120' 250 | Out-Null
$after = Info
Check '点抽屉外 · 收起' ($after -match 'drawer=0') $after

# --- 10. 马达 ---
$buzz = (Cmd 'C2_BUZZ' 300)
Check '振动马达在线' ($buzz -match 'ok=1') $buzz.Trim()

Cmd 'C2_T 9' 60 | Out-Null
$link.Close()
[C2MouseHook]::Stop()

$fail = @($results | Where-Object { -not $_.Ok }).Count
Write-Host ''
if ($fail -eq 0) {
    Write-Host ("全部通过：{0} 项" -f $results.Count) -ForegroundColor Green
    exit 0
}
Write-Host ("失败 {0} / {1} 项" -f $fail, $results.Count) -ForegroundColor Red
exit 1
