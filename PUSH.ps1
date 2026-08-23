<#
    One-shot push to a new GitHub repository, from Windows.

        .\PUSH.ps1 YOUR-GITHUB-USERNAME [repo-name]

    Create the repository on github.com first, EMPTY -- no README, licence or
    .gitignore. This tree already has all three and would conflict with them.

    The host test suite needs gcc and make, which Windows does not have, so it
    is skipped here. The included GitHub Actions workflow runs it on push
    instead, so it still gets checked -- just on GitHub rather than locally.
#>

param(
    [Parameter(Mandatory = $true)][string]$User,
    [string]$Name = "vitastremio"
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "git is not installed." -ForegroundColor Red
    Write-Host "Install it with:  winget install --id Git.Git -e"
    exit 1
}

# --- refuse to publish anything that looks personal ----------------------
# Cheap to check, and the one thing that is genuinely awkward to undo once
# it is public.
Write-Host "== checking for personal data ==" -ForegroundColor Cyan

$patterns = @(
    'aiostreams\.thegoat',
    'thegoattechnician',
    '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}'
)
$found = Get-ChildItem -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '\\\.git\\' -and
        $_.FullName -notmatch '\\release\\' -and
        $_.Name -ne 'PUSH.ps1' -and $_.Name -ne 'PUSH.sh'
    } |
    Select-String -Pattern $patterns -List

if ($found) {
    Write-Host "!! found something that looks personal:" -ForegroundColor Red
    $found | ForEach-Object { Write-Host "   $($_.Path):$($_.LineNumber)" }
    Write-Host "Review before pushing." -ForegroundColor Red
    exit 1
}
Write-Host "   clean" -ForegroundColor Green

# --- push ----------------------------------------------------------------
Write-Host "== pushing ==" -ForegroundColor Cyan

if (-not (Test-Path .git)) { git init -q }

# Belt and braces alongside .gitattributes: make sure this checkout does not
# rewrite line endings on the way in.
git config core.autocrlf false

git add .
git commit -q -m "vitastremio v1.0 beta" 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "   nothing new to commit" }

git branch -M main
git remote remove origin 2>$null | Out-Null
git remote add origin "https://github.com/$User/$Name.git"
git push -u origin main

git tag -f v1.0-beta -m "v1.0 beta" | Out-Null
git push -f origin v1.0-beta

Write-Host ""
Write-Host "Done: https://github.com/$User/$Name" -ForegroundColor Green
Write-Host "To attach the vpk as a downloadable release:"
Write-Host "  Releases -> Draft a new release -> tag v1.0-beta -> attach"
Write-Host "  release\vitastremio-v1.0-beta.vpk -> mark as pre-release"
