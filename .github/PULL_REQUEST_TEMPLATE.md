## What & why

<!-- What does this change, and what problem does it solve? Link the issue if there is one. -->

## Tested on

<!-- Device + firmware you actually ran this on. "Builds but untested on hardware" is fine to
     say — just say it. -->

- Device:
- Firmware:

## Log

<!-- Paste the relevant excerpt from KOBOeReader/.adds/nickel-type-fix/nickel-type-fix.log for
     the affected flow (the startup block plus the lines around your change). -->

```text

```

## Checklist

- [ ] `CHANGELOG.md` has an entry under the heading for the version this will ship in (required for user-visible changes).
- [ ] New or changed hooks and lookups carry a `//nb <kind> <role|*> <first> <last|*> <symbol>...` annotation.
- [ ] All hooks/dlsyms remain `.optional = true` and are null-checked at the use site.
- [ ] `./build.sh` succeeds.
