pio run
pio run --target upload

debug:
Get-PnpDevice | Where-Object { $_.Present -eq $true -and $_.InstanceId -like "USB\VID*" } | Select-Object Status, FriendlyName, InstanceId