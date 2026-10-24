# Tools

Scripts used by the build ([docs/building.md](../docs/building.md)). Some names are in Spanish; the translation is
in brackets.

| File | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty`. The repository only keeps the files this project changed. |
| `fase1_extraer.py` ("phase 1: extract") | Extracts the Xbox 360 ISO into `assets/game_root` and prints the XEX information. |
| `codegen.sh` | Runs the code generator, then `llamadas_directas.py` and `copia_literal.py`. |
| `llamadas_directas.py` ("direct calls") | Turns calls between recompiled functions into direct calls, except calls to hooked functions. Run after every code generation. |
| `copia_literal.py` ("literal copy") | Writes literal copies of five game functions; the guards of their native versions compare against them. Run after every code generation. |
| `huecos.py`, `huecos_excluir.txt` ("gaps") | Finds code the analysis left without a function and writes it to `app/huecos.toml`. |
| `lee_antes.py` ("read before") | Lists, per function, the non-volatile registers it reads before writing them. |
| `editions/` | Address matching and tree creation for porting the hooks to other editions of the game. |
