## What changed and why

<!-- One or two paragraphs. Link the issue if there is one. -->

## Checklist

- [ ] `scripts/format_sources.sh --check` passes
- [ ] `python3 tools/presets/gen_factory.py --check` passes (if presets changed)
- [ ] Relevant headless tests run locally; new behaviour has a test
- [ ] No allocation, locks, logging, or I/O added to the audio thread
- [ ] Any new third-party code or data is listed in `docs/licenses/THIRD_PARTY_NOTICES.md` with its licence
