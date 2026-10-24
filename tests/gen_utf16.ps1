$path = 'd:\Code\Y2KMeter\source\ui\toys\ToyEmoji.cpp'
$content = [System.IO.File]::ReadAllText($path)

# 匹配 u"..." 里的一串 \uXXXX（当前源文件里是 surrogate-pair 形式）
$regex = [regex]'u"((?:\\u[0-9A-Fa-f]{4})*)"'

$result = $regex.Replace($content, {
    param($m)
    $units = $m.Groups[1].Value -replace '\\u',''
    $count = $units.Length / 4
    $u16bytes = New-Object byte[] ($count * 2)
    for ($i = 0; $i -lt $count; $i++) {
        $unit = [Convert]::ToInt32($units.Substring($i * 4, 4), 16)
        $u16bytes[$i * 2] = $unit -band 0xFF
        $u16bytes[$i * 2 + 1] = ($unit -shr 8) -band 0xFF
    }
    $s = [System.Text.Encoding]::Unicode.GetString($u16bytes)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('u"')
    $i = 0
    while ($i -lt $s.Length) {
        $cp = [char]::ConvertToUtf32($s, $i)
        if ($cp -gt 0xFFFF) {
            [void]$sb.Append('\U{0:X8}' -f $cp)
            $i += 2
        } else {
            [void]$sb.Append('\u{0:X4}' -f $cp)
            $i += 1
        }
    }
    [void]$sb.Append('"')
    return $sb.ToString()
})

[System.IO.File]::WriteAllText('d:\Code\Y2KMeter\kSpecs_new.txt', $result)
Write-Output 'done'
