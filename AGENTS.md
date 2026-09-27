# Project development rules

Read [docs/CODING_STYLE.md](docs/CODING_STYLE.md) before changing production code.

- Use the repository `.clang-format` with clang-format 19; do not introduce Qt APIs or dependencies.
- Do not use `goto`. Keep lambdas short and synchronous; use named functions, members or callable
  objects for stored callbacks, asynchronous work and substantial processing.
- Serialize JSON through typed structures at the protocol boundary. Do not extract JSON fields by
  searching text or assemble JSON documents with string concatenation.
- Every owned production C/C++ source and header must contain at most 800 physical lines, including
  comments and blank lines. Split by responsibility. Tests, third-party and generated files are exempt
  from the line limit.
- Separate validation, preparation, processing, submission and cleanup with meaningful blank lines.
- Run `python3 tools/check-code-style.py` and the tests relevant to changed behavior. Keep tests and
  probes out of production sources and packages.

For third-party Prism application UI/package work, use the project skill at
[docs/skills/prism-app-ui/SKILL.md](docs/skills/prism-app-ui/SKILL.md). It routes to current visual,
Host/DSL/module contracts and a standalone starter; see [docs/README.md](docs/README.md) for the
document index. Read only the references needed for the task.
