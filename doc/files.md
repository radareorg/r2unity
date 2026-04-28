## Platform Layouts

The path detector knows these common layouts:

- iOS:
  - `Game.app/Frameworks/UnityFramework.framework/UnityFramework`
  - `Game.app/UnityFramework`
  - `Game.app/Data/Managed/Metadata/global-metadata.dat`
  - `Game.app/Data/Raw/Managed/Metadata/global-metadata.dat`
- macOS:
  - `Game.app/Contents/MacOS/<main>`
  - `Game.app/Contents/Frameworks/GameAssembly.dylib`
  - `Game.app/Contents/Resources/Data/il2cpp_data/Metadata/global-metadata.dat`
- Windows:
  - `Game.exe`
  - `GameAssembly.dll`
  - `Game_Data/il2cpp_data/Metadata/global-metadata.dat`
- Linux:
  - `Game`
  - `GameAssembly.so`
  - `Game_Data/il2cpp_data/Metadata/global-metadata.dat`
- Android extracted APK:
  - `lib/<abi>/libil2cpp.so`
  - `assets/bin/Data/Managed/Metadata/global-metadata.dat`
  - `assets/bin/Data/Managed/Metadata/global-metadata.dat.so`
  - native binary and `global-metadata.dat` in the same directory.
