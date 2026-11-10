# ?????? GBK ??????????? ?? ? UTF-8???? BOM??
$files = Get-ChildItem -Recurse -File 'd:\???\???????\host-app\src','d:\???\???????\host-app\src-tauri\src','d:\???\???????\host-app\src-tauri\Cargo.toml','d:\???\???????\host-app\src-tauri\tauri.conf.json','d:\???\???????\host-app\index.html','d:\???\???????\host-app\package.json','d:\???\???????\host-app\vite.config.ts','d:\???\???????\host-app\README.md','D:\programme\ESP32\Working_Place\Three_SimpleKey\SimpleKey_Debug\main' -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.c','.h','.rs','.toml','.json','.tsx','.ts','.html','.md','.css','.txt' -and $_.FullName -notmatch '\\(build|managed_components|target)\\' }
$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$gbk = [System.Text.Encoding]::GetEncoding(936)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

foreach ($f in $files) {
    if ($f.Extension -in '.png','.ico','.icns') { continue }
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    try {
        $null = $utf8Strict.GetString($bytes)
        Write-Output "OK    $($f.FullName)"
    } catch {
        # GBK ?? UTF-8
        $text = $gbk.GetString($bytes)
        [System.IO.File]::WriteAllText($f.FullName, $text, $utf8NoBom)
        Write-Output "FIXED $($f.FullName)"
    }
}
