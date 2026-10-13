# Security policy

XDroid360 runs untrusted code by design: it translates and executes Xbox 360 game code and parses
disc images, executables and save data. Bugs in those paths can be security bugs.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub's
[private vulnerability reporting](https://github.com/TheCafeGuy/XDroid360/security/advisories/new)
("Report a vulnerability" on the Security tab). Do not open a public issue for a vulnerability.

Include the build (shown in the app and logged at boot as `XDroid360: build <hash>`), the device and
Android version, and the steps or a minimal file that reproduces it. Never attach game files, keys,
firmware or other copyrighted material; describe how to produce the input instead.

## Scope

- In scope: code in this repository (`xdroid-core/`, `xdroid-app/` and the emulator under `src/`),
  for example memory corruption reachable from a crafted disc image or executable, path traversal
  out of the app's storage, or unsafe handling of user-installed driver packages.
- Out of scope: bugs in third-party GPU drivers (report those to Mesa or the vendor), and issues
  that need a rooted or otherwise compromised device.

Only the latest commit of the default branch is supported. There is no bug bounty.
