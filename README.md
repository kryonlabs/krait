# KITE

KITE is the Kryon Interactive Test Environment. It is a first-party Kryon app
that builds against an adjacent Kryon checkout.

```sh
make kite
make run
make test
make smoke
make install
```

On FreeBSD, use `gmake` instead of `make`.

Set `KRYON_DIR=/path/to/kryon` to use a non-adjacent checkout. The default is
`KRYON_DIR ?= ../kryon`.

The build produces `build/bin/kite`. `make install` installs `kite` and a
`kryon-ide` compatibility symlink under `PREFIX/bin`.

During the migration, Kryon's C `cmd/ki` remains in the Kryon repository as a
behavior reference. KITE owns the Kry-written app code, and Kryon must not take
a dependency on this repository.
