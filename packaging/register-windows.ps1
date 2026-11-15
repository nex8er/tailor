param([Parameter(Mandatory=$true)][string]$Executable)
$resolved = (Resolve-Path $Executable).Path
New-Item 'HKCU:\Software\Classes\Tailor.Firmware\shell\open\command' -Force | Out-Null
Set-Item 'HKCU:\Software\Classes\Tailor.Firmware\shell\open\command' ('"' + $resolved + '" "%1"')
foreach ($extension in @('.hex', '.bin')) {
  New-Item "HKCU:\Software\Classes\$extension\OpenWithProgids" -Force | Out-Null
  New-ItemProperty "HKCU:\Software\Classes\$extension\OpenWithProgids" -Name 'Tailor.Firmware' -Value '' -PropertyType String -Force | Out-Null
}
