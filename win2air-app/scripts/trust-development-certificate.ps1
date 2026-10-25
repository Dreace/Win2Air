$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Importing the public development certificate requires an elevated Windows PowerShell.'
}
$appRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))
$certificatePath = [IO.Path]::Combine($appRoot, 'out', 'private', 'development.pfx')
$privateCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath, '', [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
if ($privateCertificate.Subject -ne 'CN=Win2Air Development' -or !$privateCertificate.HasPrivateKey -or $privateCertificate.NotAfter -le [DateTime]::Now) {
    throw 'The controlled development certificate is invalid or expired.'
}
$publicCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($privateCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
$store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'LocalMachine')
$store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
try { $store.Add($publicCertificate) } finally { $store.Close() }
$reportPath = [IO.Path]::Combine($appRoot, 'out', 'validation', 'development-certificate-trust.json')
[IO.File]::WriteAllText($reportPath, ([ordered]@{ status = 'passed'; thumbprint = $publicCertificate.Thumbprint; store = 'LocalMachine/TrustedPeople'; contains_private_key = $false } | ConvertTo-Json))
$publicCertificate.Dispose()
$privateCertificate.Dispose()
