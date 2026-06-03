# Kart acik + ifconfig eth0 192.168.2.99 yapildiktan sonra:
#   powershell -ExecutionPolicy Bypass -File deploy_to_board.ps1

$Board = "192.168.2.99"
$User  = "root"
$Pass  = "root"
$Plink = "C:\Program Files\PuTTY\plink.exe"
$Pscp  = "C:\Program Files\PuTTY\pscp.exe"
$HostKey = ""  # ilk SSH'ta fingerprint alinir, asagida guncellenir

$Local = Split-Path -Parent $MyInvocation.MyCommand.Path
$RemoteDir = "/root/tiny_yolo_pynqz2"

Write-Host "[1/4] Ping $Board ..."
if (-not (Test-Connection $Board -Count 2 -Quiet)) {
    Write-Host "[HATA] Kart yanit vermiyor."
    Write-Host "  - SD kart PYNQ'de mi? Boot=SD, guc=REG"
    Write-Host "  - PuTTY seri: ifconfig eth0 192.168.2.99"
    Write-Host "  - PC IP: 192.168.2.10 mask 255.255.255.0"
    exit 1
}

if (-not $HostKey) {
    Write-Host "[2/4] SSH host key aliniyor..."
    $err = & $Plink -ssh "${User}@${Board}" -pw $Pass -batch echo OK 2>&1 | Out-String
    if ($err -match 'ssh-ed25519 255 (SHA256:[^`]+)') {
        $HostKey = $Matches[1]
        Write-Host "  HostKey: $HostKey"
    } else {
        Write-Host $err
        exit 1
    }
}

$hkArg = if ($HostKey) { @("-hostkey", $HostKey) } else { @() }

Write-Host "[3/4] Dosyalar gonderiliyor..."
& $Pscp -pw $Pass @hkArg `
    "$Local\programs\tiny_yolo_video.cpp" `
    "$Local\programs\tiny_yolo_web.cpp" `
    "$Local\makefile_video" `
    "$Local\makefile_web" `
    "$Local\run_on_board.sh" `
    "$Local\run_yolo_web.sh" `
    "${User}@${Board}:${RemoteDir}/"

& $Pscp -pw $Pass @hkArg `
    "$Local\programs\tiny_yolo_video.cpp" `
    "$Local\programs\tiny_yolo_web.cpp" `
    "${User}@${Board}:${RemoteDir}/programs/"

Write-Host "[4/4] Derleme ve calistirma..."
$cmd = @"
cd $RemoteDir
mkdir -p objects programs
chmod +x run_on_board.sh 2>/dev/null
make -f makefile_web clean
make -f makefile_web 2>&1 | tail -5
ls -la /dev/video0 2>/dev/null || echo 'KAMERA: /dev/video0 yok'
echo '--- Web: bash run_yolo_web.sh -> http://192.168.2.99:8080 ---'
"@

& $Plink -ssh "${User}@${Board}" -pw $Pass -batch @hkArg $cmd

Write-Host ""
Write-Host "[OK] Tarayici: http://${Board}:8080"
Write-Host "     Kartta: cd $RemoteDir && bash run_yolo_web.sh"
