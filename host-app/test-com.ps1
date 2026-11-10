# COM14 ??????????DTR ???? + ???? status ???? + ??????
$ErrorActionPreference = 'Stop'
try {
    $p = New-Object System.IO.Ports.SerialPort('COM14', 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $p.ReadTimeout = 800
    $p.DtrEnable = $true
    $p.RtsEnable = $true
    $p.Open()
    Write-Host 'OPENED, DTR=RTS=true'
    Start-Sleep -Milliseconds 300
    $p.Write("status`n")
    Write-Host 'SENT: status'
    $buf = New-Object byte[] 512
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $total = 0
    while ($sw.Elapsed.TotalSeconds -lt 3) {
        try {
            $n = $p.Read($buf, 0, $buf.Length)
            $total += $n
            Write-Host ('RX: ' + [Text.Encoding]::UTF8.GetString($buf, 0, $n).Trim())
        } catch [TimeoutException] {}
    }
    Write-Host ('TOTAL BYTES: ' + $total)
    $p.Close()
} catch {
    Write-Host ('ERR: ' + $_.Exception.Message)
}
