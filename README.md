# Krait

Krait is the standalone Kryon IDE. It is a first-party Kryon app that builds
against its vendored Kryon submodule.

```sh
make krait
make run
make dev ARGS=/path/to/project
make test
make smoke
make install
```

On FreeBSD, use `gmake` instead of `make`.

Run `git submodule update --init --recursive` after cloning. Set
`KRYON_DIR=/path/to/kryon` to build against a different checkout, or use
`make dev` to build and run against `../kryon`. The default is
`KRYON_DIR ?= vendor/kryon`. The executable also accepts `--dev` and
`--kryon-dir /path/to/kryon` so runtime tools such as live preview builds use the
same Kryon checkout.

The build produces `build/bin/krait`. `make install` installs the `krait`
binary under `PREFIX`. Krait uses the Noto Sans font bundled with Kryon.

Krait owns the Kry-written IDE app code. Kryon must not take a dependency on
this repository.
