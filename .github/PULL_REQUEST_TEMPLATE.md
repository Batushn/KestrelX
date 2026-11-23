## Summary

<!-- Describe what changed and why. -->

Closes #

## Checklist

- [ ] **Clean-Room & MIT:** 100% original clean-room code (or MIT-compatible), zero proprietary SDK headers, binaries, keys, or retail dumps (`CONTRIBUTING.md`).
- [ ] **MSVC `/W4 /permissive-`:** `powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Config Release` builds with 0 warnings.
- [ ] **Test Suite:** `.\build\Release\kestrel_tests.exe` passes (and `--unit-only` passes cleanly without creating `.bmp` files in `docs/`).
- [ ] **CLI Selftest:** `.\build\Release\kestrel.exe selftest` passes.
- [ ] **Guest Test Title:** `.\build\Release\kestrel.exe run build\Release\test_title\kestrel_test_title.exe` passes.
