# Krait

Krait is the standalone Kryon IDE. It is a first-party Kryon app that builds
against an adjacent Kryon checkout.

```sh
make krait
make run
make test
make smoke
make install
```

On FreeBSD, use `gmake` instead of `make`.

Set `KRYON_DIR=/path/to/kryon` to use a non-adjacent checkout. The default is
`KRYON_DIR ?= ../kryon`.

The build produces `build/bin/krait`. `make install` installs `krait` and its
bundled UI font under `PREFIX`. See `THIRD_PARTY_NOTICES.md` for bundled asset
notices.

Krait owns the Kry-written IDE app code. Kryon must not take a dependency on
this repository.
