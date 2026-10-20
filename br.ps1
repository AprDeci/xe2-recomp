param(
    [switch]$BuildOnly,
    [switch]$NoBuild,
    [ValidateRange(1, 64)][int]$Jobs = 12
)

$ErrorActionPreference = 'Stop'
if($BuildOnly -and $NoBuild){ throw 'Choose either -BuildOnly or -NoBuild.' }
$root = $PSScriptRoot
$exe = Join-Path $root 'build\ssbu.exe'
$modules = Join-Path $root 'dump\modules'

Push-Location $root
try {
    if(!$NoBuild){
        foreach($name in 'main', 'sdk', 'subsdk0'){
            if(!(Test-Path -LiteralPath "exefs_base\$name")){ throw "Missing exefs_base\$name. See README.md for setup." }
        }
        if(!$env:INCLUDE -or !$env:LIB){
            $vcvars = $env:VCVARS64
            if(!$vcvars){
                $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
                if(Test-Path -LiteralPath $vswhere){
                    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
                    if($LASTEXITCODE -eq 0 -and $vs){ $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
                }
            }
            if(!$vcvars -or !(Test-Path -LiteralPath $vcvars)){
                throw 'Install Visual Studio C++ tools, use an x64 Developer PowerShell, or set VCVARS64 to vcvars64.bat.'
            }
            $vsEnv = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
            if($LASTEXITCODE -ne 0){ throw 'Visual Studio environment setup failed.' }
            foreach($line in $vsEnv){
                if($line -match '^([^=]+)=(.*)$'){ Set-Item -LiteralPath "env:$($matches[1])" -Value $matches[2] }
            }
        }
        $python = Join-Path $root '.venv\Scripts\python.exe'
        if(!(Test-Path -LiteralPath $python)){ throw 'Create .venv and install requirements.txt as described in README.md.' }
        New-Item -ItemType Directory -Path $modules -Force | Out-Null
        & $python tools\recomp.py all exefs_base --out build --jobs $Jobs
        if($LASTEXITCODE -ne 0){ throw "Build failed (exit $LASTEXITCODE)." }
    }
    if(!$BuildOnly){
        foreach($path in $exe, (Join-Path $modules 'main.reloc.bin'), (Join-Path $modules 'sdk.reloc.bin'), (Join-Path $modules 'subsdk0.reloc.bin'), (Join-Path $modules 'romfs.cfg')){
            if(!(Test-Path -LiteralPath $path)){ throw "Missing $path. See README.md for setup." }
        }
        # Run from the project root so local configuration and relative RomFS paths stay portable.
        & $exe $modules
        if($LASTEXITCODE -ne 0){ throw "XC2 exited with code $LASTEXITCODE." }
    }
} finally {
    Pop-Location
}
