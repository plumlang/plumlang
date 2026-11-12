# Security Policy

## Supported versions

Before 1.0, only the latest release and `main` get security fixes. From 1.0
on:

| Version | Supported |
|---|:-:|
| latest `v*` release | ✅ |
| previous minor release | ✅ (critical only) |
| older | ❌ |

## Reporting a vulnerability

Please **don't** open a public issue for security problems.

Use one of:

1. **GitHub Security Advisory** (preferred):
   <https://github.com/plumlang/plumlang/security/advisories/new>
2. **Email:** `hello@elixpo.com`

You can expect:

- An acknowledgement within **3 working days**.
- A first triage opinion ("we agree this is X severity") within **7 days**.
- A fix or mitigation plan within **30 days**, faster for high-severity
  issues. We'll keep you in the loop the whole way.
- Credit in the release notes (unless you'd rather stay anonymous).

## Scope

In scope:

- Memory-safety bugs in the runtime reachable from a Plumlang program
  (out-of-bounds access, use-after-free, double free, uninitialised reads).
- Escapes from any sandbox or permission the runtime promises.
- Bugs in the `net` and `http` modules that let a remote peer crash the
  process, read its memory or run code.
- Malicious input to the compiler (a source file) that corrupts memory.
- Tampering with official release binaries or the install scripts.

Out of scope (but still please tell us, just lower priority):

- A Plumlang program that deliberately exhausts memory or loops forever.
- Bugs in third-party programs written in Plumlang.
