# Driver signature allowlist

Before JiYuTrainer asks the Service Control Manager to start a kernel driver,
the user-mode loader validates the file with `WinVerifyTrust`. Embedded
Authenticode signatures are checked first; installed Windows catalogs are used
as a fallback for catalog-only drivers.

A driver is normally accepted only when both conditions are true:

1. Windows reports a valid certificate chain for the exact file or catalog
   member.
2. The verified leaf certificate's simple display name exactly matches one of
   the 53 entries in `DriverPublisherAllowlist.h`, ignoring case only. The
   comparison does not normalize punctuation, whitespace, or other characters:
   `VMware, Inc` and `VMware, Inc.` are separate publishers, while
   `VMware, Inc..` and `VMware, Inc ` are rejected.

The two private JiYu test-driver publishers are the only exception to the
chain-trust requirement. For those exact names, `CERT_E_UNTRUSTEDROOT` is
accepted because the test certificates use a private root. Missing signatures,
bad digests, expired certificates, and all other trust failures remain fatal.
Third-party allowlist entries still require a fully trusted chain.

The check covers newly created services, existing services opened by
`MLoadKernelDriver`, and the `XTryReuseInstalledDriver` path. A rejection is
logged with the trust status, publisher when available, service, and path;
`StartService` is not called.

`DriverPublisherTests` tests exact allowlist matching. The policy probe runs the
same production verifier against a driver:

```powershell
DriverPublisherTests\Release\DriverSignaturePolicyProbe.exe C:\Windows\System32\drivers\disk.sys
```

This policy controls drivers started by JiYuTrainer. System-wide driver
admission remains the responsibility of Windows Code Integrity or WDAC.

## Kernel driver guard (last-resort blocking)

The user-mode allowlist above only gates drivers that JiYuTrainer itself
starts. The driver guard extends the same publisher allowlist to every
kernel driver loaded by *any* component after JiYuTrainer runs, as a
fallback against anti-tamper drivers reloading.

1. On connect, `DriverGuard.cpp` evaluates each candidate in this order:
   Windows protected driver locations are accepted first; then the exact
   signing publisher is compared with the static publisher table; then a
   valid publisher is checked against the dynamic registry list at
   `HKLM\SOFTWARE\JiYuTrainer\DriverPublisherAllowlist`; finally the file
   MD5 is checked against `HKLM\SOFTWARE\JiYuTrainer\DriverMd5Allowlist`.
   Anything that reaches the end of the chain is untrusted and is not added.
   Accepted files are pushed as `(lowercase basename, SHA-256)` pairs to the
   driver with `CTL_DRIVER_GUARD_CONFIG`. The guard is armed after the fast
   first stage.
2. In the load-image notify callback (which runs before the image's
   `DriverEntry`), `DriverGuard64.c` checks every kernel-mode image against
   the whitelist. A whitelisted name whose on-disk file no longer hashes to
   the recorded SHA-256 is treated as unlisted.
3. An unlisted driver's `DriverEntry` is rewritten in memory through an MDL
   alias (read-only PTEs are not modified) to the 8-byte stub
   `48 C7 C0 22 00 00 C0 C3` (`mov rax, 0FFFFFFFFC0000022h; ret`), so the
   loader fails the load with `STATUS_ACCESS_DENIED`. The file on disk is
   never modified. Each block is reported through the event stream as
   `JDRV_EVENT_TYPE_DRIVER_BLOCKED` with the reason in `reserved`.

Guard state, entry count, and block statistics are queryable through
`CTL_DRIVER_GUARD_QUERY`. Disarming uses `CTL_DRIVER_GUARD_CONFIG` with
`JDRV_GUARD_ACTION_DISARM`; `CLEAR` also disarms.

The runtime log reports `interception=effective` and
`whitelist=effective` only after a successful kernel query with `armed=1` and
at least one whitelist entry. It also reports counts accepted by each trust
path and the number rejected before being sent to the kernel.

Scope limits: drivers loaded before the trainer starts are not affected
(this is a post-load fallback, not a boot-time gate). Systems with HVCI
enabled cannot load the unsigned trainer driver at all, so the MDL alias
write is only exercised where kernel page protection is not hypervisor
enforced. A driver that spoofs a whitelisted basename from a different
directory is caught by the SHA-256 check when the file is readable; if the
file cannot be hashed, the load is allowed to avoid breaking legitimate
PnP loads.
