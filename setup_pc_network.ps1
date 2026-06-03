# PYNQ Ethernet: PC = 192.168.2.10, kart = 192.168.2.99
# Yonetici olarak calistir: powershell -ExecutionPolicy Bypass -File setup_pc_network.ps1

$AdapterName = "Ethernet"
$Ip = "192.168.2.10"
$Mask = "255.255.255.0"

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $admin) {
    Write-Host "Yonetici yetkisi isteniyor (UAC -> Evet)..."
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', $MyInvocation.MyCommand.Path
    )
    exit
}

Write-Host "[1] Ethernet IP ayarlaniyor: $Ip / $Mask"
netsh interface ip set address name="$AdapterName" static $Ip $Mask 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[UYARI] netsh basarisiz. Adaptör adi farkli olabilir."
    Get-NetAdapter | Format-Table Name, Status, LinkSpeed
    exit 1
}

Write-Host "[2] Ping kart (192.168.2.99)..."
Start-Sleep -Seconds 1
if (Test-Connection 192.168.2.99 -Count 2 -Quiet) {
    Write-Host "[OK] Kart erisilebilir."
    Write-Host "     YOLO pano: http://192.168.2.99:8080"
} else {
    Write-Host "[UYARI] Ping yok. Kartta: ifconfig eth0 192.168.2.99"
    Write-Host "        Kablo + REG guc + DPU SD kart kontrol et."
}
