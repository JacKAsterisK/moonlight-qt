# Repository workflow

Work directly on the existing `master` branch. Do not create branches or branch-based worktrees unless the user explicitly requests them.

Keep automatic CI separate from manually requested Windows distribution builds. The Reprint Hub repository owns the manual BuildMachine package and publication workflows.

Windows source builds use `setup.bat` and `build.bat`; `update.bat` closes existing Moonlight instances before rebuilding. Distribution packages use `package.bat` and install per user through `install.bat`. Verify installer changes in an isolated temporary folder, without closing the user's running Moonlight session.
