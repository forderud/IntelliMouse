# Get IntelliMouse WMI object
# HW drivers reside in the "root\wmi" namespace (https://learn.microsoft.com/en-us/windows/win32/wmicoreprov/wdm-provider)
$mouse = Get-CimInstance -Namespace root\WMI -Class MouseMirrorDeviceInformation

Write-Host("IntelliMouse device:")
Write-Host("  InstanceName: {0}" -f $mouse.InstanceName)
Write-Host("  Active: {0}" -f $mouse.Active)

Write-Host("  Flipping: LeftRight={0}, UpDown={1}" -f $mouse.FlipLeftRight, $mouse.FlipUpDown)

Write-Host("Enabling flipping of mouse movement...")
$mouse.FlipLeftRight = $true
$mouse.FlipUpDown = $true

Write-Host("Storing changes...")
Set-CimInstance -CimInstance $mouse
