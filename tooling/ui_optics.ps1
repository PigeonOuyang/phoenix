# SPDX-License-Identifier: Unlicense
[CmdletBinding(DefaultParameterSetName = 'Set')]
param(
    [Parameter(Mandatory, Position = 0, ParameterSetName = 'Set')]
    [ValidateRange(80, 2160)][int]$Diameter,
    [Parameter(ParameterSetName = 'Set')]
    [ValidateRange(4, 128)][int]$Stroke = 48,
    [Parameter(ParameterSetName = 'Set')]
    [ValidateRange(0, 2160)][int]$X = 1080,
    [Parameter(ParameterSetName = 'Set')]
    [ValidateRange(0, 2160)][int]$Y = 1080,
    [Parameter(ParameterSetName = 'Set')]
    [ValidateRange(30, 1800)][int]$Seconds = 1200,
    [Parameter(Mandatory, ParameterSetName = 'Stop')][switch]$Stop,
    [Parameter(Mandatory, ParameterSetName = 'Status')][switch]$Status,
    [string]$Endpoint = 'root@172.16.42.1',
    [string]$Session = (Join-Path $env:TEMP 'p4-breeze-native-device-20261125')
)
$ErrorActionPreference = 'Stop'
$sshArgs = @('-o','BatchMode=yes','-o','ConnectTimeout=3',
    '-o','StrictHostKeyChecking=yes','-o',"UserKnownHostsFile=$(Join-Path $Session 'known_hosts')",
    '-i',(Join-Path $Session 'id_ed25519'),'--',$Endpoint)
$frame = 'cat /run/overpressure/ui-frame.json'
$request = 'cd /run/overpressure && umask 077 && '
if ($PSCmdlet.ParameterSetName -eq 'Set') {
    if ($Stroke * 4 -gt $Diameter -or $Diameter / 2 -gt [Math]::Min([Math]::Min($X,2160-$X),[Math]::Min($Y,2160-$Y))) {
        throw 'The entire ring must fit on each panel; the stroke must be at most one quarter of the diameter.'
    }
    $id = [Guid]::NewGuid().ToString('N').Substring(0,16)
    $request += 'uptime=$(cut -d. -f1 /proc/uptime) && '
    $request += "tmp=.optics-$id && "
    $request += ('printf ''%s %s %s %s %s %s\n'' {0} {1} {2} {3} {4} "$((uptime+{5}))" > "$tmp" && mv -f "$tmp" optics-ring && ' -f $id,$Diameter,$Stroke,$X,$Y,$Seconds)
    $condition = ('grep -Fq ''"{0}"'' ui-frame.json' -f $id)
} elseif ($PSCmdlet.ParameterSetName -eq 'Stop') {
    $request += 'rm -f optics-ring && '
    $condition = 'grep -Eq ''"optics_ring"[[:space:]]*:[[:space:]]*null'' ui-frame.json'
} else {
    $request = ''
}
if ($request) {
    $request += "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do if $condition; then break; fi; sleep .1; done; "
}
$raw = & ssh @sshArgs ($request + $frame)
if ($LASTEXITCODE -ne 0) { throw 'Recovery SSH request failed.' }
$state = ($raw -join "`n") | ConvertFrom-Json
if (!$state.PSObject.Properties['optics_ring']) { throw 'This running Recovery does not support the optics ring.' }
if ($PSCmdlet.ParameterSetName -eq 'Set' -and $state.optics_ring.id -ne $id) {
    throw 'The requested ring was not acknowledged by KMS.'
}
if ($Stop -and $null -ne $state.optics_ring) { throw 'Recovery did not acknowledge removal of the ring.' }
[pscustomobject]@{ Frame=$state.frame; Page=$state.page; Ring=$state.optics_ring }
