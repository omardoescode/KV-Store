# KV-Store
A unecessarily elaborate KV Store. Might be better than DynamoDB

## Development

Requires `gcc`, a C++ compiler (only for the H2O build), `make`, `clang-format`, `clang-tidy`, `cmake`, `pkg-config`, `valgrind`, `curl`, and the OpenSSL and zlib development files.

HTTP server: [H2O](https://github.com/h2o/h2o) (`libh2o-evloop`), a git submodule in `third_party/h2o`. `make` builds it with CMake into `build/h2o/`. Supported: Linux, macOS, BSD.

Only `src/http/` can include H2O. All other code uses the wrapper in `src/http/http.h`; see `src/main.c` for an example.

JSON: [yyjson](https://github.com/ibireme/yyjson), a git submodule in `third_party/yyjson`. Only `src/json/` can include it. All other code uses the wrapper in `src/json/json.h`, which has no dependency on the HTTP wrapper.

After clone, run the following command to make sure all needed commands are installed, the submodule is fetched, and git hooks are enabled

```sh
make init
```

The pre-commit hook checks format, runs clang-tidy on staged files, and runs `make memcheck`. CI (`.github/workflows/ci.yml`) runs the same checks on each pull request.


| Command | Function |
|---|---|
| `make` | Build `build/app`, write `build/compile_commands.json` for clangd |
| `make test` | Build and run the server on `127.0.0.1:7890` (`curl 127.0.0.1:7890/ping`) |
| `make memcheck` | Run the server under valgrind, send requests, fail on a memory error or leak. Port 7890 must be free |
| `make format` | Format sources |
| `make format-check` | Check format, no changes |
| `make lint` | Run clang-tidy |
| `make lint-fix` | Run clang-tidy and apply fixes |
| `make clean` | Remove the app build, keep the H2O build |
| `make distclean` | Remove `build/` |
