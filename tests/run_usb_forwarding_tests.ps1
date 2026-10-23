# Builds and runs the USB forwarding unit tests with qmake + MSVC.
#
#   .\tests\run_usb_forwarding_tests.ps1 -OutDir C:\somewhere\outside\the\repo [-Tests name,name]
#
# Everything uses loopback sockets and throwaway certificates generated into
# OutDir. Nothing is installed, no service or device is touched.
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string[]]$Tests = @('usb_forwarding_capability', 'usb_forwarding_environment',
                         'usb_forwarding_backend_list', 'usb_forwarding_tunnel', 'usb_forwarding_tunnel_clientcert',
                         'usb_forwarding_nvhttp'),
    [string]$QtDir = 'C:\Qt\6.10.3\msvc2022_64',
    [string]$VcVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    [string]$OpenSsl = 'C:\Program Files\Git\usr\bin\openssl.exe'
)

$ErrorActionPreference = 'Stop'
$testsRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testsRoot '..')).Path
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
# Compare with a trailing separator so a sibling such as "<repo>-verify" is not
# mistaken for a path inside "<repo>".
$sep = [System.IO.Path]::DirectorySeparatorChar
$repoPrefix = $repoRoot.TrimEnd('\', '/') + $sep
if (($OutDir.TrimEnd('\', '/') + $sep).StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutDir must be outside the repository.'
}

# Throwaway certificates: "a" is the paired host, "b" is an impostor.
$certDir = Join-Path $OutDir 'certs'
New-Item -ItemType Directory -Force $certDir | Out-Null
foreach ($name in 'a', 'b') {
    $cert = Join-Path $certDir "cert-$name.pem"
    $key = Join-Path $certDir "key-$name.pem"
    if (-not (Test-Path $cert) -or -not (Test-Path $key)) {
        # openssl prints key-generation progress on stderr; Windows PowerShell
        # would treat that as an error under 'Stop'.
        $ErrorActionPreference = 'Continue'
        & $OpenSsl req -x509 -newkey rsa:2048 -nodes -keyout $key -out $cert -days 2 `
            -subj "/CN=usbfwd-test-$name" -addext "subjectAltName=IP:127.0.0.1" 2>&1 | Out-Null
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { throw "openssl failed for $name" }
    }
}
$env:USBFWD_TEST_CERTS = $certDir

# PATH without any OpenSSL DLL directory, so Qt keeps Schannel (the Windows
# default) unless a test asks for OpenSSL.
$basePath = ($env:PATH -split ';' | Where-Object {
        $_ -and -not (Test-Path (Join-Path $_ 'libssl-3-x64.dll')) -and -not (Test-Path (Join-Path $_ 'libssl-1_1-x64.dll'))
    }) -join ';'
$failed = @()
foreach ($test in $Tests) {
    $src = Join-Path $testsRoot $test
    $out = Join-Path $OutDir $test
    New-Item -ItemType Directory -Force $out | Out-Null
    $log = Join-Path $out 'build.log'
    $cmd = "call `"$VcVars`" >nul && cd /d `"$out`" && `"$QtDir\bin\qmake.exe`" `"$src\$test.pro`" " +
           "`"CONFIG+=release`" `"CONFIG-=debug_and_release`" && nmake /nologo"
    cmd /c "$cmd > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BUILD FAILED: $test (see $log)"
        Get-Content $log -Tail 25
        $failed += $test
        continue
    }
    $exe = Get-ChildItem -Path $out -Recurse -Filter '*_test.exe' | Select-Object -First 1
    # OpenSSL DLLs (from Git for Windows) go last so they never shadow Qt's own.
    $gitRoot = Split-Path (Split-Path (Split-Path $OpenSsl))
    $env:PATH = "$QtDir\bin;" + $basePath
    # Schannel is the Windows default; only the client-certificate test needs
    # OpenSSL (Schannel servers never request a client certificate).
    $env:QT_TLS_BACKEND = 'schannel'
    if ($test -like '*clientcert*') {
        $env:PATH += ";" + (Join-Path $gitRoot 'mingw64\bin')
        $env:QT_TLS_BACKEND = 'openssl'
    }
    $result = Join-Path $out 'result.txt'
    & $exe.FullName -o "$result,txt"
    $code = $LASTEXITCODE
    Write-Host "== $test (exit $code)"
    if (Test-Path $result) {
        Get-Content $result | Where-Object { $_ -match '^(FAIL|SKIP|Totals)|Actual|Expected|failure location' } | Out-Host
    }
    if ($code -ne 0) { $failed += $test }
}
if ($failed.Count -gt 0) {
    Write-Host "FAILED: $($failed -join ', ')"
    exit 1
}
Write-Host 'ALL PASSED'
