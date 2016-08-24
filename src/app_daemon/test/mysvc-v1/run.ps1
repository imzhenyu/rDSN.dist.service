
$port = $args[0];
$package_dir = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition

Add-Content run.txt "package_dir = $package_dir"
Add-Content run.txt "port = $port"

$cmd = "dsn.svchost"
$params = "$package_dir\config.ini -cargs port=$port";
Start-Process $cmd $params -Wait

