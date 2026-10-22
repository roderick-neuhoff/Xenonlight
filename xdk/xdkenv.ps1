# Sets up the Xbox 360 SDK command-line tools for this PowerShell session.
#   . .\xdk\xdkenv.ps1
# XEDK may point at an SDK install or an extracted copy (XDK folder with
# bin\win32, include\xbox and lib\xbox).
#
# Uses the SDK's "Jul12" technology-preview compiler (cl 16.10) because it
# ships a complete set of public CRT headers and CRT libraries; in the
# installer, the main compiler's CRT headers only exist as CRT-build-internal
# copies. The SDK's own headers and libraries come from include\xbox / lib\xbox.
if (-not $env:XEDK) {
    $env:XEDK = "C:\AI\xbox\dev kits\XBOX360 SDK 21256.3\extracted\XDK"
}
$jul12 = "$env:XEDK\TechPreview\Jul12Compiler"
if (-not (Test-Path "$jul12\bin\win32\cl.exe")) {
    throw "Xbox 360 compiler not found under XEDK=$env:XEDK"
}
# The Jul12 bin folder has only the compiler and linker; imagexex etc. come from bin\win32.
$env:PATH = "$jul12\bin\win32;$env:XEDK\bin\win32;$env:PATH"
$env:INCLUDE = "$jul12\include\xbox;$env:XEDK\include\xbox"
$env:LIB = "$jul12\lib\xbox;$env:XEDK\lib\xbox"
