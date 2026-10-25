param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [Parameter(Mandatory = $true)][string]$SdkTools,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$appRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))
$privateRoot = [IO.Path]::Combine($appRoot, 'out', 'private')
$packagePath = [IO.Path]::Combine($appRoot, 'out', 'msix', $Configuration, 'Win2Air.msix')
[IO.Directory]::CreateDirectory($privateRoot) | Out-Null
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
& icacls.exe $privateRoot /inheritance:r /grant:r "*$($identity):(OI)(CI)F" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not restrict the developer certificate directory to the current user.' }
$keyPath = [IO.Path]::Combine($privateRoot, 'development.pfx')
if (!(Test-Path -LiteralPath $keyPath)) {
    $rsa = [Security.Cryptography.RSA]::Create(3072)
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=Win2Air Development', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new([Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
    $usage = [Security.Cryptography.OidCollection]::new()
    $usage.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3')) | Out-Null
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usage, $true))
    $certificate = $request.CreateSelfSigned([DateTimeOffset]::Now.AddMinutes(-5), [DateTimeOffset]::Now.AddYears(1))
    [IO.File]::WriteAllBytes($keyPath, $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx, ''))
    $certificate.Dispose()
    $rsa.Dispose()
}
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($keyPath, '', [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
if ($certificate.Subject -ne 'CN=Win2Air Development' -or !$certificate.HasPrivateKey -or $certificate.NotAfter -le [DateTime]::Now) {
    throw 'The controlled developer certificate is invalid or expired.'
}
& ([IO.Path]::Combine($SdkTools, 'signtool.exe')) sign /fd SHA256 /f $keyPath $packagePath
if ($LASTEXITCODE -ne 0) { throw "MSIX signing failed: $LASTEXITCODE" }
$installed = $false
$family = $null
if ($Install) {
    $store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'LocalMachine')
    $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
    try {
        $trusted = $store.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $certificate.Thumbprint, $false)
        if ($trusted.Count -ne 1) {
            throw 'Trust the public development certificate using scripts/trust-development-certificate.ps1 in an elevated Windows PowerShell before MSIX installation.'
        }
    } finally { $store.Close() }
    Add-AppxPackage -Path $packagePath -ForceUpdateFromAnyVersion -ErrorAction Stop
    $package = Get-AppxPackage -Name 'Win2Air.Desktop' -ErrorAction Stop
    if (!$package -or $package.Architecture -ne 'X64' -or $package.Publisher -ne $certificate.Subject) {
        throw 'Installed package identity or architecture did not match the built App.'
    }
    $family = $package.PackageFamilyName
    & ([IO.Path]::Combine($SdkTools, 'signtool.exe')) verify /pa $packagePath
    if ($LASTEXITCODE -ne 0) { throw "Installed package signature verification failed: $LASTEXITCODE" }
    & ([IO.Path]::Combine($appRoot, 'out', 'tests', $Configuration, 'Win2Air.Tests.exe')) --activate "$family!App"
    if ($LASTEXITCODE -ne 0) { throw "Installed App window validation failed: $LASTEXITCODE" }
    $installed = $true
}
Write-Output "Package signed; installation validation: $installed"
$certificate.Dispose()
