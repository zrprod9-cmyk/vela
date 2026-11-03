## What this changes

<!-- One or two sentences. Link the issue: Fixes #123 -->

## Part of Vela
- [ ] plugin (`plugin/`)
- [ ] OptiScaler fork patches (`fork/`)
- [ ] RE4 interceptor (`interceptor/`)
- [ ] NPU layer (`npu-layer/`) or NPU system part (`npu/`)
- [ ] docs only

## How it was tested
<!-- Device and armadaOS build, game(s), model, what you saw (FPS with conditions, logs, VELA-XXXXXX report numbers).
     Plugin changes: `python tests/test_<name>.py` for each file in `plugin/tests`. -->

## Checklist
- [ ] Builds (`pnpm install && pnpm build`, `build_zip.py` for the plugin; `fork/README.md` steps for the fork)
- [ ] Panel texts added in `plugin/src/i18n/en.json` (the panel is English only)
- [ ] No keys, tokens, personal paths or game files in the diff
- [ ] I agree to license my contribution under GPL-3.0 (see LICENSING.md)
