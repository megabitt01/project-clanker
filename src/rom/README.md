# Game files go here

This repository contains no game code or assets. To build and run, copy the
contents of your own legitimately-dumped **Star Wars Battlefront II (Xbox)**
disc into this directory so that it looks like:

```
src/rom/
  default.xbe          (5,132,288 bytes, SHA-1 35ac5bd9ad3879f4a9c5eb5b306163e7b67f8964)
  Data/
    dsstdfx.bin
    xboxeffects.bin
    _LVL_XBOX/
      common.lvl  core.lvl  ingame.lvl  ...
```

CMake refuses to configure when `default.xbe` is missing or does not match the
expected hash. Every address the port hooks belongs to that exact executable.

The runtime loads the original machine code from `default.xbe` at startup and
serves `Data/` as the Xbox `D:` drive. Nothing in this folder is ever modified.
