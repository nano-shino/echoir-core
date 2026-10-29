# Download the models listed in models/registry.json.
#
# Mirror of tools/fetch-models.sh, for Windows (Windows PowerShell 5.1 or PowerShell 7).
#
# Usage: tools\fetch-models.ps1 <id> [<id> ...]
#
# - Idempotent: a file already in models\ with the registry size is skipped.
# - Downloads to <name>.part and renames on success, so a finished file is never partial.
# - An interrupted download keeps its .part and resumes on the next run.
# - A size mismatch against the registry fails with a non-zero exit code.
# - An entry with convert_from whose hosted file is not there (HTTP 401 or 404) is not downloaded
#   here. The script prints the commands that fetch the upstream checkpoints and convert them with
#   tools\convert-firered-vad.py, then fails.
# - Works from any directory: the repo root is resolved from this script's location.
# - Uses curl.exe, which ships with Windows 10 (1803) and later.

[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Ids
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Registry = Join-Path $Root 'models\registry.json'
$ModelsDir = Join-Path $Root 'models'

function Fail([string]$Message) {
    [Console]::Error.WriteLine("error: $Message")
    exit 1
}

# Byte size of a file, or -1 when it does not exist.
function Get-FileSize([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [long](Get-Item -LiteralPath $Path).Length
    }
    return -1L
}

# Run curl.exe with the given arguments and return its exit code.
function Invoke-Curl([string[]]$CurlArgs) {
    & curl.exe @CurlArgs
    return [int]$LASTEXITCODE
}

# HTTP status of a GET of $Url with the body discarded; "000" when the server is unreachable.
function Get-HttpStatus([string]$Url) {
    $code = & curl.exe --silent --location --range 0-0 --output NUL --write-out '%{http_code}' $Url
    return [string]$code
}

# Print the commands that build a registry file from the upstream checkpoints (convert_from).
# The paths are absolute, so the commands work from any directory.
function Show-UpstreamConversion([string]$ModelId, [string]$Name, $Upstream) {
    $dir = Join-Path $ModelsDir ".firered\$ModelId"
    $converter = Join-Path $Root 'tools\convert-firered-vad.py'
    $output = Join-Path $ModelsDir $Name
    Write-Host "$Name is not hosted yet. Build it from the upstream checkpoints:"
    Write-Host "  New-Item -ItemType Directory -Force `"$dir`" | Out-Null"
    Write-Host "  curl.exe --location --output `"$dir\model.pth.tar`" $($Upstream.model)"
    Write-Host "  curl.exe --location --output `"$dir\cmvn.ark`" $($Upstream.cmvn)"
    Write-Host "  python -I `"$converter`" `"$dir\model.pth.tar`" `"$dir\cmvn.ark`" `"$output`""
}

# Download one registry file into models\, verifying its size.
function Get-ModelFile([string]$Name, [string]$Url, [long]$Want, [string]$Role, [string]$ModelId, $Upstream) {
    $dest = Join-Path $ModelsDir $Name
    $part = "$dest.part"

    if ((Get-FileSize $dest) -eq $Want) {
        Write-Host ("ok        {0} ({1}, already present)" -f $Name, $Role)
        return
    }

    $have = Get-FileSize $part
    if ($have -gt $Want) {
        Write-Host "discard   $Name.part (larger than the registry size)"
        Remove-Item -LiteralPath $part -Force
        $have = -1L
    }

    if ($have -ne $Want) {
        Write-Host "download  $Name ($Role, $Want bytes)"
        $rc = 0
        $fresh = $true
        if ($have -gt 0) {
            Write-Host "          resuming at byte $have"
            $rc = Invoke-Curl @('--fail', '--location', '--retry', '3', '--retry-delay', '2', '-C', '-', '-o', $part, $Url)
            if ($rc -eq 33) {
                # curl exit 33: the server ignored the Range header. Start over.
                Write-Host "          server cannot resume; restarting"
                Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
                $rc = 0
            }
            else {
                $fresh = $false
            }
        }
        if ($fresh -and $rc -eq 0) {
            $rc = Invoke-Curl @('--fail', '--location', '--retry', '3', '--retry-delay', '2', '-o', $part, $Url)
        }
        if ($rc -ne 0) {
            # curl reports some HTTP errors with other exit codes, so ask for the status.
            if ($Upstream -and ((Get-HttpStatus $Url) -in @('401', '404'))) {
                Show-UpstreamConversion $ModelId $Name $Upstream
                Fail "$Name is not hosted (HTTP 401 or 404); run the commands above"
            }
            Fail "download failed (curl exit $rc): $Url; any partial file is kept at $part, rerun to resume"
        }
    }

    $got = Get-FileSize $part
    if ($got -ne $Want) {
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        Fail "size mismatch for ${Name}: got $got bytes, registry says $Want (partial removed)"
    }

    Move-Item -LiteralPath $part -Destination $dest -Force
    Write-Host "done      $Name"
}

if (-not (Test-Path -LiteralPath $Registry -PathType Leaf)) {
    Fail "registry not found: $Registry"
}
$Reg = Get-Content -LiteralPath $Registry -Raw -Encoding UTF8 | ConvertFrom-Json
$Known = @($Reg.models | ForEach-Object { $_.id })

if (-not $Ids -or $Ids.Count -eq 0) {
    Write-Host 'Usage: fetch-models.ps1 <id> [<id> ...]'
    Write-Host 'Downloads the files of each model id into models\.'
    Write-Host 'Known ids:'
    foreach ($k in $Known) { Write-Host "  $k" }
    exit 2
}

if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
    Fail 'curl.exe was not found (it ships with Windows 10 1803 and later)'
}

foreach ($id in $Ids) {
    if ($Known -notcontains $id) {
        [Console]::Error.WriteLine("error: unknown model id '$id'")
        exit 2
    }
}

New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

foreach ($id in $Ids) {
    Write-Host "== $id"
    $model = $Reg.models | Where-Object { $_.id -eq $id } | Select-Object -First 1
    foreach ($f in $model.files) {
        Get-ModelFile -Name $f.name -Url $f.url -Want ([long]$f.size) -Role $f.role `
            -ModelId $id -Upstream $model.convert_from
    }
}

Write-Host "all requested models are in $ModelsDir"
