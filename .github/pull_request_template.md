## Summary

What changes for users?

## Safety boundary

- Which untrusted inputs or process-write controls are affected?
- Can unknown or `ReadOnlyCandidate` builds reach new behavior?
- How are IAT ownership and remote allocation lifetime preserved?

## Verification

- [ ] Added a failing test before implementation
- [ ] `build.ps1 -Analyze`
- [ ] `--self-test`
- [ ] Updated `README.md` or `docs/MAINTAINER.md` when the contract changed
- [ ] No game binary, personal log, local INI, credential, or generated artifact included
- [ ] Shareable diagnostics contain no local path, PID, address, account identifier, or raw platform detail
