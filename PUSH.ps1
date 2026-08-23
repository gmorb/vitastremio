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

# Continue, not Stop.
#
# PowerShell turns anything a native command writes to stderr into an error
# record, and with Stop that terminates the script. git writes perfectly
# normal progress and status messages to stderr -- "No such remote" on a
# first run, for one -- so Stop makes routine output fatal. Exit codes are
# checked explicitly instead, which is what actually indicates failure.
$ErrorActionPreference = "Continue"
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

# Only commit when something is actually staged; committing nothing is an
# error, and on a re-run there may be nothing.
git diff --cached --quiet
if ($LASTEXITCODE -eq 0) {
    Write-Host "   nothing new to commit"
} else {
    git commit -q -m "vitastremio v1.0 beta"
    if ($LASTEXITCODE -ne 0) { Write-Host "commit failed" -ForegroundColor Red; exit 1 }
}

git branch -M main

# Test for the remote rather than removing it blindly: on a first run there
# is none, and git says so on stderr.
$remotes = @(git remote)
if ($remotes -contains "origin") { git remote remove origin }
git remote add origin "https://github.com/$User/$Name.git"

git push -u origin main
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Push failed. Common causes:" -ForegroundColor Red
    Write-Host "  - the repository does not exist yet on github.com"
    Write-Host "  - it was created with a README, so it has commits already"
    Write-Host "    (fix: git pull --rebase origin main, then re-run)"
    Write-Host "  - the username '$User' is wrong"
    exit 1
}

git tag -f v1.0-beta -m "v1.0 beta" | Out-Null
git push -f origin v1.0-beta
if ($LASTEXITCODE -ne 0) { Write-Host "   tag push failed (not fatal)" }

Write-Host ""
Write-Host "Done: https://github.com/$User/$Name" -ForegroundColor Green
Write-Host "To attach the vpk as a downloadable release:"
Write-Host "  Releases -> Draft a new release -> tag v1.0-beta -> attach"
Write-Host "  release\vitastremio-v1.0-beta.vpk -> mark as pre-release"
