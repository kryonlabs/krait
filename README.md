# Krait

Krait is the standalone Kryon IDE. It is a first-party Kryon app that builds
against its vendored Kryon submodule.

## Modes

Krait has several modes (View menu or the top-right dropdown):

- **Studio** — visual screen editing, source editor, live preview,
  inspector and problems tooling in one dockable workbench.
- **Game Engine** — a 2D game editor on the Kryon Game2D scene tree:
  a scene document (`game.scene` per project, scaffolded into new
  projects), node palette (Node2D, Sprite2D, AnimatedSprite2D,
  Camera2D, Body2D, Area2D, TileMap, Timer, AudioSource), a playable
  viewport with Box2D physics, pluggable behaviors (the built-in player,
  spin and patrol register through the same public behavior-registry API
  games use, each with named parameters editable in the inspector),
  cameras, an inspector with tile painting,
  and Play/Pause/Stop. Area2D and Timer nodes fire through the Kryon
  signal bus with triggers (collect, win, score) so pickups and level
  exits work out of the box. AnimationPlayer nodes animate any node's
  position/rotation/scale with multi-track keyframe tweens edited in
  the inspector. Stop rebuilds the scene from the
  document, so editing never fights the simulation. Run it with
  `krait --view game`.
- **Running outside the editor**: `Run Game` launches the scene in the
  standalone player (`krait --play-game <scene-or-project>`, own window,
  no editor chrome; ESC quits, F5 restarts). `Export` writes a
  self-contained `<project>/game-export/` folder: the scene, the player
  binary bundled under `bin/krait-player`, a `run.sh` launcher and a
  `.desktop` entry — it runs with no krait installation present. The
  player also runs scaffolded projects directly.
- **Agent**, **Kanban**, **Text**, **Settings** — the coding agent chat,
  the agentic board, distraction-free editing, and app settings.

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

The build produces `build/<platform>-<arch>/bin/krait` (e.g.
`build/linux-x86_64/bin/krait`). `make install` installs the `krait`
binary under `PREFIX`. Krait uses the Noto Sans font bundled with Kryon.

Krait owns the Kry-written IDE app code. Kryon must not take a dependency on
this repository.
