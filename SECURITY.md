# Security Policy

## Reporting a vulnerability

WinStellar ships Windows Explorer shell extensions (thumbnail, preview and
property handlers) that parse image files **in-process inside `explorer.exe`**
the moment a user browses a folder. Parsing bugs are therefore security-relevant.

**Please report vulnerabilities privately** — do not open a public issue for a
security problem.

- Use GitHub's **[private vulnerability reporting](https://github.com/caelo-works/winstellar/security/advisories/new)**
  (the *Report a vulnerability* button under the repository's **Security** tab).
- Include: affected version, a description, reproduction steps, and — if
  possible — a minimal sample file that triggers the issue.

We aim to acknowledge a report within a few days and to ship a fix in a
subsequent release. Please give us a reasonable window to address the issue
before any public disclosure.

## Supported versions

Only the **latest release** is supported with security fixes. Please update
before reporting; the installed base has no auto-update yet (see the tracker).

## Scope

In scope: the shell extensions, the file loaders (FITS/XISF/RAW), the viewer,
and the installer. Out of scope: issues that require the attacker to already
have code execution or administrative access on the target machine.
