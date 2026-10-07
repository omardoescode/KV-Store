# KV-Store
A unecessarily elaborate KV Store. Might be better than DynamoDB

## Development

Requires `gcc`, `make`, `clang-format`, `clang-tidy`.

After clone, run the following command to make sure all needed commands are installed, and git hooks are enabled

```sh
make init
```

The pre-commit hook checks format and runs clang-tidy on staged files.


| Command | Function |
|---|---|
| `make` | Build `bin/app` |
| `make test` | Build and run |
| `make format` | Format sources |
| `make format-check` | Check format, no changes |
| `make lint` | Run clang-tidy |
| `make lint-fix` | Run clang-tidy and apply fixes |
| `make clean` | Remove `bin/` |
