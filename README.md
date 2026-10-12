# KV-Store
A unecessarily elaborate KV Store. Might be better than DynamoDB

## Development

Requires `gcc`, `make`, `clang-format`, `clang-tidy`, `cmake`, `pkg-config`, and the OpenSSL and zlib development files.

HTTP server: [H2O](https://github.com/h2o/h2o) (`libh2o-evloop`), a git submodule in `third_party/h2o`. `make` builds it with CMake into `bin/h2o/`. Supported: Linux, macOS, BSD.

Only `src/http/` can include H2O. All other code uses the wrapper in `src/http/http.h`; see `src/main.c` for an example.

After clone, run the following command to make sure all needed commands are installed, the submodule is fetched, and git hooks are enabled

```sh
make init
```

The pre-commit hook checks format and runs clang-tidy on staged files.


| Command | Function |
|---|---|
| `make` | Build `bin/app`, write `build/compile_commands.json` for clangd |
| `make test` | Build and run the server on `127.0.0.1:7890` (`curl 127.0.0.1:7890/ping`) |
| `make format` | Format sources |
| `make format-check` | Check format, no changes |
| `make lint` | Run clang-tidy |
| `make lint-fix` | Run clang-tidy and apply fixes |
| `make clean` | Remove the app build, keep the H2O build |
| `make distclean` | Remove `bin/` and `build/` |
