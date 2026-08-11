# Krait Modules

Krait keeps language and tool support outside the IDE core. Built-in modules
live under `modules/<id>/` and register through `ide/modules.kry`.

A module owns language-specific behavior such as:

- file type detection
- syntax selection
- build and live preview hooks
- diagnostics parsing
- project templates

Core IDE code should ask `ide/modules.kry` for those answers instead of checking
language-specific extensions directly.

The bundled `modules/kryon` module is the reference implementation. It is
enabled by default so Krait still opens as a full Kryon IDE, but it can be
disabled from Settings > Extensions.

The current registry is statically linked. Keep new modules in this source-tree
shape so the same API can later be exposed to dynamically loaded modules.
