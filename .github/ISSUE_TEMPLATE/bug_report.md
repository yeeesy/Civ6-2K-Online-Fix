---
name: Bug report
about: Report a reproducible launcher, detection, UI, or safety failure
title: "[Bug] "
labels: bug
---

## Environment

- Tool version and EXE SHA-256:
- Windows version:
- Steam game version:
- Renderer actually launched: DX11 / DX12 / unknown
- Mode: GUI / watch-only / no-gui / diagnose

## What happened

Describe the observed and expected behavior. State whether the UI ever said that a write occurred.

## Reproduction

Start from a fully stopped game and list exact steps.

## Diagnostics

- Tool final result and game menu's actual online/offline state:
- Reached the main menu: yes / no / unknown
- Frequency: always / intermittent / once
- Did the tool start listening before the game was launched?
- Mods enabled: yes / no / unknown
- Cache/database changes already tried: none / describe only the action, filename and anonymized relative location. Do not delete anything to reproduce this report.

Paste the GUI's sanitized “复制脱敏诊断” summary when available. Do not paste raw JSONL. If `--diagnose` output is essential, include only the smallest relevant excerpt after removing local usernames/paths, PIDs, memory/module addresses, account identifiers, and unrelated hashes. Never include passwords, Steam/2K tokens, or a game executable.

For diagnostic format 2, include the complete text (result code, field read quality, timings and bounded timeline), not only the last three numbers. If before/after reports already exist, include both and describe what changed. Do not upload SQLite databases or account data. A timeout category is an observation, not a root-cause diagnosis. See `docs/DIAGNOSTICS.md` in the source tree.
