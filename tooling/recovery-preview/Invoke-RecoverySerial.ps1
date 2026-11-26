# SPDX-License-Identifier: Unlicense
param([Parameter(Mandatory=$true)][string]$Command, [int]$Seconds=12)
$ErrorActionPreference='Stop'
$ports=@(Get-CimInstance Win32_PnPEntity -Filter "ClassGuid='{4d36e978-e325-11ce-bfc1-08002be10318}'" |
    Where-Object {$_.PNPDeviceID -match 'VID_1D6B&PID_0104' -and $_.Name -match '\(COM\d+\)'})
if($ports.Count -ne 1){throw 'Expected one Recovery ACM terminal'}
$port=([regex]::Match($ports[0].Name,'COM\d+')).Value
$serial=[System.IO.Ports.SerialPort]::new($port,115200)
$serial.DtrEnable=$true
$serial.Open()
try{
    $serial.Write("`r")
    Start-Sleep -Milliseconds 450
    if($serial.ReadExisting() -match 'login:'){
        $serial.Write("root`r")
        Start-Sleep -Milliseconds 400
        $null=$serial.ReadExisting()
    }
    $serial.Write('printf "\n%s\n" REC_ACM_BEGIN; { '+$Command+'; }; printf "\n%s\n" REC_ACM_END'+"`r")
    $end=(Get-Date).AddSeconds($Seconds)
    $buffer=''
    do{
        Start-Sleep -Milliseconds 170
        $buffer+=$serial.ReadExisting()
    }until($buffer -match '(?m)^REC_ACM_END\r?$' -or (Get-Date) -ge $end)
    $result=[regex]::Match($buffer,'(?ms)^REC_ACM_BEGIN\r?\n(.*?)^REC_ACM_END\r?$')
    if(-not $result.Success){throw 'Recovery serial command did not return its final marker'}
    $result.Groups[1].Value
}finally{
    $serial.Close()
    $serial.Dispose()
}
