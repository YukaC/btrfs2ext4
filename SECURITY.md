# Security Policy

## Supported versions

Only the `master` branch receives active fixes.

| Version | Supported |
| ------- | --------- |
| `0.2.x` | ✅        |
| `< 0.2` | ❌        |

---

## Threat model

`btrfs2ext4` runs as root and reads raw Btrfs on-disk structures. This makes it an attractive attack surface: a maliciously crafted filesystem image could potentially cause memory corruption, stack exhaustion, or arbitrary writes.

The tool treats **all Btrfs input as untrusted by default:**

- Length and bounds checks on every B-tree item before access
- Integer overflow guards via GCC `__builtin_add_overflow`
- Explicit depth caps on recursive tree traversal (no unbounded stack growth)
- A fuzz suite (`test_stress.c`) that feeds malformed headers, truncated nodes, loops, and out-of-bounds references to all reader APIs

That said — this is a hobby project built quickly. There are almost certainly gaps. If you find one, please report it.

---

## Reporting a vulnerability

If you find something exploitable — a memory corruption path, an integer overflow that bypasses the guards, a way to trick the engine into writing to unintended blocks — **please do not open a public issue.**

Send an email to: **agusyuk25@gmail.com**

Include:

1. A brief description of the vulnerability and how you found it
2. The specific file, struct, or code path involved
3. A minimal reproducer if possible (a Btrfs image or hex dump that triggers it)

I'll respond within 48 hours. Confirmed vulnerabilities will get a fix pushed immediately, and I'll file a public CVE if appropriate.
