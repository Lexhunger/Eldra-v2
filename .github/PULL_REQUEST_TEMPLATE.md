# Pull Request - Eldra-V2

## Summary
- What does this PR do? (1-3 sentences)
- What feature/bug does it address, and why does it matter?
- Anything the reviewer should know up front?

## Changes Included
- Major changes (modules, rendering, palette updates, sensor integration, docs, refactors, bug fixes)

## Testing
- `idf.py build`
- `idf.py flash monitor`
- Tested on hardware (Waveshare ESP32-S3 2.8" Round Display)
- Verified eye rendering and animation timing
- Verified sensors/IMU (if applicable)
- Verified RGB565 colors
- Verified rendering position and scaling
- Notes, screenshots, serial logs, or photos (if useful)

## Documentation
- Updated: `docs/hardware/...`, `docs/firmware/...`, `docs/reference/...`, `docs/roadmap.md`, `README`
- If no docs were required, explain why (e.g., "Small refactor; no external behavior changed.")

## AI-Generated Code (ChatGPT / Codex / Copilot)
Check ALL:
- I reviewed the entire diff manually.
- AI did not alter hardware pin mappings unexpectedly.
- AI did not change ESP-IDF version, target, or build system.
- AI did not rewrite modules outside the scope of this PR.
- Existing APIs, naming, and file structure were respected.
- Constants (screen size, palette indices, etc.) remain correct.
- Behavior was confirmed on hardware OR logically inspected.
- Large generated sections include inline comments or explanation.

Optional: Paste prompts used to generate code.

## Related Issues or Discussions
- Link issues/roadmap items (e.g., `Closes #123`, `Implements Roadmap: Phase 1 - Display Pipeline`)

## Additional Notes
- Breaking changes?
- Required follow-up tasks?
- Known limitations?
- Good to squash-merge?

## Final Review Checklist
- PR is focused with a single clear purpose
- No accidental large rewrites
- No stray debug prints (ESP_LOG*) unless intended
- Code follows Eldra-V2 conventions
- Docs updated
- Everything builds and runs
