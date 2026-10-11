param(
    [string]$QtRoot = '',
    [string]$OpenSsl = 'C:\Program Files\Git\usr\bin\openssl.exe',
    [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$TestName = ''
)
$ErrorActionPreference = 'Stop'
if ($TestName -notmatch '^[a-zA-Z0-9 -]*$') { throw 'Invalid test-name filter.' }
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $QtRoot) { $QtRoot = Join-Path $repo '.tools\Qt\6.12.0\msvc2022_64' }
foreach ($required in @($OpenSsl, $VcVars, (Join-Path $QtRoot 'bin\qmake.exe'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing integration-test tool: $required" }
}
$testRoot = Join-Path $repo ('.tools\clipboard-integration\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$utf8 = [System.Text.UTF8Encoding]::new($false)
$opensslConfig = Join-Path $testRoot 'openssl-fixture.conf'
[System.IO.File]::WriteAllText($opensslConfig, "[req]`ndistinguished_name=fixture_name`n[fixture_name]`n", $utf8)
$authorityCert = Join-Path $testRoot 'authority.pem'
$authorityKey = Join-Path $testRoot 'authority.key'
& $OpenSsl req -config $opensslConfig -x509 -newkey rsa:2048 -nodes -days 1 -sha256 -subj '/CN=clipboard-integration-authority' `
    -keyout $authorityKey -out $authorityCert -addext 'basicConstraints=critical,CA:TRUE' `
    -addext 'keyUsage=critical,keyCertSign,cRLSign' 2> (Join-Path $testRoot 'authority-generation.log')
if ($LASTEXITCODE -ne 0) { throw 'Cannot generate ephemeral HTTPS test authority.' }
foreach ($identity in @('server', 'wrong-server', 'client')) {
    $cert = Join-Path $testRoot ($identity + '.pem')
    $key = Join-Path $testRoot ($identity + '.key')
    $usage = if ($identity -eq 'client') { 'clientAuth' } else { 'serverAuth' }
    $request = Join-Path $testRoot ($identity + '.csr')
    $extensions = Join-Path $testRoot ($identity + '.ext')
    [System.IO.File]::WriteAllText($extensions, "basicConstraints=critical,CA:FALSE`nkeyUsage=critical,digitalSignature,keyEncipherment`nextendedKeyUsage=$usage`nsubjectAltName=IP:127.0.0.1,DNS:localhost`n", $utf8)
    $opensslArgs = @('req', '-config', $opensslConfig, '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256',
        '-subj', ('/CN=clipboard-integration-' + $identity), '-keyout', $key, '-out', $request)
    & $OpenSsl @opensslArgs 2> (Join-Path $testRoot ($identity + '-generation.log'))
    if ($LASTEXITCODE -ne 0) { throw 'Cannot generate ephemeral HTTPS test certificates.' }
    & $OpenSsl x509 -req -in $request -CA $authorityCert -CAkey $authorityKey -CAcreateserial `
        -days 1 -sha256 -extfile $extensions -out $cert 2>> (Join-Path $testRoot ($identity + '-generation.log'))
    if ($LASTEXITCODE -ne 0) { throw 'Cannot sign ephemeral HTTPS test certificates.' }
}
$selfSigned = Join-Path $testRoot 'self-signed-server.pem'
& $OpenSsl req -config $opensslConfig -x509 -new -key (Join-Path $testRoot 'server.key') -days 1 -sha256 `
    -subj '/CN=Sunshine' -out $selfSigned `
    2> (Join-Path $testRoot 'self-signed-server-generation.log')
if ($LASTEXITCODE -ne 0) { throw 'Cannot generate pinned self-signed HTTPS test certificate.' }
& $OpenSsl verify -CAfile $selfSigned $selfSigned > (Join-Path $testRoot 'self-signed-validation.log') 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Generated self-signed fixture certificate is invalid.' }
$batch = @"
@echo off
setlocal
call "$VcVars" >nul
if errorlevel 1 exit /b 1
set "PATH=$QtRoot\bin;$repo\libs\windows\lib\x64;%PATH%"
cd /d "$testRoot"
"$QtRoot\bin\qmake.exe" "$PSScriptRoot\clipboard_integration.pro" CONFIG+=release
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
release\test_clipboardsync.exe "$testRoot" $(if ($TestName) { '"' + $TestName + '"' })
exit /b %ERRORLEVEL%
"@
$batchPath = Join-Path $testRoot 'run-tests.bat'
[System.IO.File]::WriteAllText($batchPath, $batch.Replace("`n", "`r`n"), $utf8)
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "Clipboard HTTPS integration tests failed (exit $LASTEXITCODE)." }
Write-Host 'Tests used loopback HTTPS, ephemeral identities and in-memory clipboard callbacks only.'
