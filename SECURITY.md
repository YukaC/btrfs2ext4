# btrfs2ext4 Security Policy

## Supported Versions

Currently only the `master` branch is receiving proactive security audits and hardening updates.

| Version | Supported          |
| ------- | ------------------ |
| `0.1.x` | :white_check_mark: |
| `< 0.1` | :x:                |

---

## Threat Model

`btrfs2ext4` operates as a filesystem metadata parser and binary format translator. Crucially, the tool often operates with root/sudo privileges (required to access raw block devices `BLKGETSIZE64` operations), meaning an exploitation could compromise the host OS.

The tool intentionally assumes **all Btrfs data structures are unconditionally malicious, corrupted, or crafted by an attacker**.

The engine implements several explicit defenses to combat this:

1. **Mathematical Fuzzing Protection:** The `test_fuzz.c` suite continuously mutates B-Tree bounds, depth metrics, CRC payloads, and overlapping physical extents to actively seek crash states or out-of-bounds `memcpy` behavior.
2. **Zero-Trust Input Parsing:** Length checks, bounds clipping, stack/buffer overflow limits, and explicit integer overflow prevention mechanisms (via GCC `__builtin_add_overflow`) intercept corrupt physical size claims to avoid exploitable execution logic.
3. **Hardened Tree Traversal:** Recursive structures are mitigated via explicit stack-bounded loops (DFS max array trackers), protecting against deep stack exhaustion crashes intended to disrupt kernel memory boundaries.

---

## Reporting a Vulnerability

If you've found a way to reliably execute arbitrary code via a malformed filesystem image, discover an integer overflow path we missed, or determine a mechanism capable of bypassing the ASan boundary checks and tricking the engine into destroying an unintended disk—**please do not open a public issue.**

Instead, please send an immediate, detailed email covering the attack vector or vulnerability to:

📧 **agusyuk25@gmail.com**

**Please include:**

1. A brief description of the vulnerability and attack vector.
2. The specific file, struct, or API call exhibiting the flaw.
3. (Crucial) Instructions on how to reproduce the issue, or ideally, a minimal Btrfs hex-dump/image that triggers the error context.

You should receive a response within 48 hours. Validated vulnerabilities will result in an immediate hotfix push, followed by a public CVE disclosure if appropriate. Thank you for making file conversions safer for everyone!
