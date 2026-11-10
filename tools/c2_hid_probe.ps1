#!/usr/bin/env pwsh
<#
.SYNOPSIS
    端到端验证 Core2 Keypad 的 HID 上报：串口触发按键，Windows 全局钩子确认系统收到。

.DESCRIPTION
    专治「主机显示已连接、但按键没反应」这类只在重连后才出现的毛病。流程：
      1. 挂 WH_KEYBOARD_LL 全局低级键盘钩子
      2. （-Reset）拉低 RTS 硬重置 Core2，等它自动重连回这台电脑
      3. 串口发 C2_CCCD 看订阅位，再发 K_UP / K_DOWN
      4. 检查钩子是否收到 vk=38（↑）/ vk=40（↓）

    退出码 0 = 两个方向键都到；1 = 至少一个没到。
    运行期间自己敲键盘也会被记录（这是全局钩子），报告里按 vk 区分。

.EXAMPLE
    pwsh tools/c2_hid_probe.ps1                 # 只测当前连接
    pwsh tools/c2_hid_probe.ps1 -Reset          # 先硬重置再测，复现「reset 后失联」
    pwsh tools/c2_hid_probe.ps1 -Port COM3
#>
param(
    [string]$Port = 'COM6',
    [int]$Baud = 921600,
    [switch]$Reset
)

$ErrorActionPreference = 'Stop'

$hookSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class C2KeyHook
{
    delegate IntPtr LowLevelKeyboardProc(int nCode, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct KBDLLHOOKSTRUCT
    {
        public uint vkCode;
        public uint scanCode;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int ptX;
        public int ptY;
    }

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int idHook, LowLevelKeyboardProc lpfn, IntPtr hMod, uint dwThreadId);
    [DllImport("user32.dll")]
    static extern bool UnhookWindowsHookEx(IntPtr hhk);
    [DllImport("user32.dll")]
    static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern bool PeekMessage(out MSG lpMsg, IntPtr hWnd, uint min, uint max, uint remove);
    [DllImport("user32.dll")]
    static extern bool TranslateMessage(ref MSG lpMsg);
    [DllImport("user32.dll")]
    static extern IntPtr DispatchMessage(ref MSG lpMsg);
    [DllImport("kernel32.dll", CharSet = CharSet.Auto)]
    static extern IntPtr GetModuleHandle(string name);

    const int WH_KEYBOARD_LL = 13;
    const uint PM_REMOVE = 1;

    static readonly LowLevelKeyboardProc _proc = HookCallback;
    static IntPtr _hook = IntPtr.Zero;
    static Thread _thread;
    static volatile bool _running;

    public static readonly List<string> Events = new List<string>();

    static IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            int msg = wParam.ToInt32();
            if (msg == 0x0100 || msg == 0x0101)
            {
                KBDLLHOOKSTRUCT d = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));
                bool injected = (d.flags & 0x10) != 0;
                lock (Events)
                    Events.Add(string.Format("{0} vk={1} scan=0x{2:X2}{3}",
                        msg == 0x0100 ? "DOWN" : "UP", d.vkCode, d.scanCode, injected ? " injected" : ""));
            }
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    public static bool Start()
    {
        _running = true;
        _thread = new Thread(delegate ()
        {
            _hook = SetWindowsHookEx(WH_KEYBOARD_LL, _proc, GetModuleHandle(null), 0);
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
if (-not [C2KeyHook]::Start()) { throw 'SetWindowsHookEx 失败，无法挂全局键盘钩子' }
Write-Host '键盘钩子已挂上' -ForegroundColor DarkGray

$link = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$link.DtrEnable = $false      # DTR 低 = GPIO0 高，不会进下载模式
$link.RtsEnable = $false
$link.ReadTimeout = 300
try {
    $link.Open()
} catch {
    [C2KeyHook]::Stop()
    throw "无法打开 $Port：$($_.Exception.Message)"
}

function Send-Command([string]$cmd, [int]$wait = 400) {
    $link.Write("$cmd`n")
    Start-Sleep -Milliseconds $wait
    return $link.ReadExisting().Trim()
}

try {
    Start-Sleep -Milliseconds 400
    $link.DiscardInBuffer()

    if ($Reset) {
        Write-Host '重新硬化 reset（RTS -> EN）...' -ForegroundColor DarkGray
        $link.RtsEnable = $true; Start-Sleep -Milliseconds 120; $link.RtsEnable = $false

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $log = New-Object System.Text.StringBuilder
        $connected = $false
        while ($sw.ElapsedMilliseconds -lt 20000 -and -not $connected) {
            $chunk = $link.ReadExisting()
            if ($chunk) {
                [void]$log.Append($chunk)
                if ($log.ToString() -match '\[BLE\] connected') { $connected = $true }
            }
            Start-Sleep -Milliseconds 20
        }
        if (-not $connected) { Write-Warning '20 秒内没等到 [BLE] connected，主机可能没有自动重连' }
        Start-Sleep -Milliseconds 800
        $link.DiscardInBuffer()
        foreach ($line in ($log.ToString() -split "`n")) {
            $i = $line.IndexOf('[')                      # 跳过 ROM boot 的乱码前缀
            if ($i -ge 0) {
                $tail = $line.Substring($i).Trim()
                if ($tail -match '^\[(BLE|CCCD|SEC)\]') { Write-Host "  $tail" -ForegroundColor DarkGray }
            }
        }
    }

    function Show-Tap([string]$reply) {
        return (@($reply -split "`n") | Where-Object { $_ -match '\[KEY\] press|C2_TAP|C2_ERROR' } | ForEach-Object { $_.Trim() }) -join ' | '
    }

    Write-Host ''
    Write-Host ('C2_CCCD  -> ' + (Send-Command 'C2_CCCD')) -ForegroundColor Cyan
    [C2KeyHook]::Events.Clear()
    Write-Host ('K_UP     -> ' + (Show-Tap (Send-Command 'K_UP' 700))) -ForegroundColor Cyan
    Write-Host ('K_DOWN   -> ' + (Show-Tap (Send-Command 'K_DOWN' 700))) -ForegroundColor Cyan
    Start-Sleep -Milliseconds 600
} finally {
    $link.Close()
}

$events = @([C2KeyHook]::Events)
[C2KeyHook]::Stop()

Write-Host ''
Write-Host '--- Windows 收到的事件 ---'
if ($events.Count -eq 0) { Write-Host '(none)' -ForegroundColor Red } else { $events | ForEach-Object { Write-Host "  $_" } }

$up   = @($events | Where-Object { $_ -match '^DOWN vk=38 ' }).Count -gt 0
$down = @($events | Where-Object { $_ -match '^DOWN vk=40 ' }).Count -gt 0

Write-Host ''
if ($up -and $down) {
    Write-Host 'PASS: ↑ / ↓ 都到达 Windows' -ForegroundColor Green
    exit 0
}
Write-Host ("FAIL: ↑={0} ↓={1}" -f $up, $down) -ForegroundColor Red
Write-Host '如果 C2_CCCD 显示 kb=0000，就是订阅位丢了（notify 会被静默丢弃）' -ForegroundColor Yellow
exit 1
