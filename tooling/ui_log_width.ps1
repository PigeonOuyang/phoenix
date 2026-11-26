# SPDX-License-Identifier: Unlicense
[CmdletBinding(DefaultParameterSetName = 'Set')]
param(
    [Parameter(Mandatory, Position = 0, ParameterSetName = 'Set')]
    [ValidateRange(960, 2040)][int]$Width,
    [Parameter(Mandatory, ParameterSetName = 'Interactive')][switch]$Interactive,
    [Parameter(Mandatory, ParameterSetName = 'Reset')][switch]$Reset,
    [Parameter(Mandatory, ParameterSetName = 'Status')][switch]$Status,
    [string]$Endpoint = 'root@172.16.42.1',
    [string]$Session = (Join-Path $env:TEMP 'p4-breeze-native-device-20261125')
)

$ErrorActionPreference = 'Stop'
$sshArgs = @('-o','BatchMode=yes','-o','ConnectTimeout=3',
    '-o','StrictHostKeyChecking=yes','-o',"UserKnownHostsFile=$(Join-Path $Session 'known_hosts')",
    '-i',(Join-Path $Session 'id_ed25519'),'--',$Endpoint)

function Get-LogFrame([int]$Target = 0, [switch]$RemoveOverride) {
    $command = 'cd /run/overpressure && cut -d. -f1 /proc/uptime > host-contact && '
    if ($RemoveOverride) {
        $command += 'rm -f log-width && '
        $Target = 1970
    } elseif ($Target) {
        if ($Target % 2) { throw 'Width must be an even number of native pixels.' }
        $command += ('umask 077 && t=$(mktemp .log-width.XXXXXX) && printf ''%s\n'' {0} > "$t" && mv "$t" log-width && ' -f $Target)
    }
    if ($Target) {
        $command += ('for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -Eq ''"log_width_px"[[:space:]]*:[[:space:]]*{0}[[:space:]]*[,}}]'' ui-frame.json && break; sleep .1; done; ' -f $Target)
    }
    $raw = & ssh @sshArgs ($command + 'cat ui-frame.json')
    if ($LASTEXITCODE -ne 0) { throw 'Recovery SSH request failed.' }
    $s = ($raw -join "`n") | ConvertFrom-Json
    if (!$s.PSObject.Properties['log_width_px']) { throw 'This running Recovery does not support changing the log width.' }
    if ($Target -and $s.log_width_px -ne $Target) { throw 'Recovery did not acknowledge the requested width.' }
    [pscustomobject]@{ Page=$s.page; Width=$s.log_width_px; TextWidth=$s.log_text_width_px;
        VisibleLines=$s.log_visible_lines; Frame=$s.frame }
}

if ($Interactive) {
    $preview = Get-LogFrame
    $preview | Format-Table
    Write-Host 'Left/Right: -/+40 px; Up/Down: +/-10 px; Enter/Esc: keep and exit; R: reset.'
    while ($true) {
        $pressed = [Console]::ReadKey($true)
        if ($pressed.Key -in @('Enter','Escape')) { break }
        if ($pressed.Key -eq 'R') {
            $preview = Get-LogFrame -RemoveOverride
        } else {
            $step = switch ($pressed.Key) {
                LeftArrow { -40 }; RightArrow { 40 }; UpArrow { 10 }; DownArrow { -10 }; default { 0 }
            }
            if (!$step) { continue }
            $target = [Math]::Clamp($preview.Width + $step,960,2040)
            if ($target -eq $preview.Width) { continue }
            $preview = Get-LogFrame $target
        }
        $preview | Format-Table
    }
} elseif ($Reset) {
    Get-LogFrame -RemoveOverride
} elseif ($Status) {
    Get-LogFrame
} else {
    Get-LogFrame $Width
}
