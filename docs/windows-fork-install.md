# Moonlight fork for Windows

Install the already built client through Reprint Hub's **Remote development** page, or extract the complete deployment bundle and run `install.bat` on the client PC. Build tools and administrator rights are not required.

The installer verifies the payload's SHA-256 receipt, checks its paths and x64 executable, closes the current Windows user's running Moonlight instances, then activates the new binaries under `%LOCALAPPDATA%\MoonlightFork\current`. It adds a **Moonlight Fork** Start menu shortcut. Existing Moonlight settings and pairings stay in the normal Windows user profile. An upstream MSI installation can remain installed; the fork has its own executable and shortcut.

Updates interrupt active streams. The previous binaries remain in `backup-*` folders below `%LOCALAPPDATA%\MoonlightFork`. If activation fails, the installer restores the previous `current` folder. Do not rename `portable.dat.inactive` in a managed installation: its inactive state keeps settings outside the replaceable binary folder. An unrelated portable Moonlight installation keeps its own separate profile.

The ZIP contains exactly `Moonlight.zip`, `Moonlight.install.json`, `install.bat`, `README.md`, `scripts/windows/moonlight-windows.ps1`, and `scripts/windows/MoonlightInstall.psm1`. The nested payload includes the application, Qt plugins, codecs and Visual C++ runtime DLLs. Hashes identify the delivered bytes; they are not an Authenticode publisher signature.

## Clipboard and tablet options

In Moonlight **Settings → Input Settings**, enable text clipboard sync, copied file/folder sync and/or **Stream Wacom tablet pressure and tilt (WinTab)**, then start a new stream. These options are off by default. Clipboard sync requires the updated Sunshine fork with **Clipboard Text Sync** and/or **Clipboard File and Folder Transfer** enabled on the host. File copies become available to paste only after the transfer completes; cut does not delete the source. See the [clipboard guide](https://github.com/JacKAsterisK/moonlight-qt/blob/master/docs/windows-clipboard-sync.md).

Tablet capture uses the existing Wacom driver on the client and native Windows pen input on the host. In Blender, select **Preferences → Input → Tablet API → Windows Ink**. PhotoCraft must likewise accept Windows pen input. See the [tablet guide](https://github.com/JacKAsterisK/moonlight-qt/blob/master/docs/windows-tablet-input.md) for display mapping and compatibility details.

## Building and checking a package

Run `setup.bat` once in the source checkout, then `package.bat`. Packaging builds Release x64, verifies the payload and runs `Moonlight.exe --version` without compiler or Qt directories in PATH. The result is `build\package-x64-release\Moonlight-fork-windows.zip`, with a separate build receipt in the same directory. Clean source builds are required for Hub publication.

Set `ML_QT_ROOT` to reuse an existing Qt installation and `ML_BUILD_JOBS` to limit jom parallelism. `MOONLIGHT_NO_PAUSE=1` disables the batch pause for automation. The ordinary `build.bat` still writes `bin\Moonlight.exe`; the source `update.bat` still pulls, closes Moonlight and rebuilds that developer binary.

To verify a copied payload without installing it:

```powershell
powershell.exe -NoProfile -File scripts/windows/moonlight-windows.ps1 -Action Smoke -PayloadPath C:\path\Moonlight.zip
```

The receipt must be adjacent to `Moonlight.zip`. Installation and update use the same `install.bat` entrypoint. This package does not configure Sunshine, VPNs, networking, Windows firewall rules or tablet drivers.
