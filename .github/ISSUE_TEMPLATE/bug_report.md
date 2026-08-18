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

Paste the GUI's sanitized “复制脱敏诊断” summary when available. Do not paste raw JSONL. If `--diagnose` output is essential, include only the smallest relevant excerpt after removing local usernames/paths, PIDs, memory/module addresses, account identifiers, and unrelated hashes. Never include passwords, Steam/2K tokens, or a game executable.
