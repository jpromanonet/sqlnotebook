﻿# Before running this script, publish the ClickOnce installer in Visual Studio.
# ..\publish\ will be populated.

Remove-Item ..\publish-portable -Recurse -ErrorAction SilentlyContinue
mkdir ..\publish-portable

# folder: 'SqlNotebook_0_4_0_0'
$folder = (dir '..\publish\Application Files' | ForEach {$_.Name} | Sort {$_} | Select -Last 1)
# absFolder: 'C:\Projects\sqlnotebook\publish\Application Files\SqlNotebook_0_4_0_0'
$absFolder = (Resolve-Path ('..\publish\Application Files\' + $folder))

copy "$absFolder\*.*" ..\publish-portable\
dir ..\publish-portable\ | ForEach {Rename-Item $_.FullName $_.FullName.Replace(".deploy", "")}
del ..\publish-portable\SqlNotebook.application
del ..\publish-portable\SqlNotebook.exe.manifest

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$inFolder = (Resolve-Path ..\publish-portable)
$outZip = [System.IO.Path]::Combine((Resolve-Path ..\publish), $folder + ".zip")
Remove-Item "..\publish\$folder.zip" -ErrorAction SilentlyContinue
[System.IO.Compression.ZipFile]::CreateFromDirectory($inFolder, $outZip)

Remove-Item ..\publish-portable -Recurse -ErrorAction SilentlyContinue