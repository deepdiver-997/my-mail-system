# ProtoRelay Project Style Guide

This document captures conventions inspired by large production-grade CLI/network projects.

## 1. Naming and Versioning

- Product name: ProtoRelay.
- Versioning: Semantic Versioning (MAJOR.MINOR.PATCH), e.g. 0.1.0.
- Build metadata should be injected at configure/build time (version, commit, target, compiler).

## 2. CLI Contract

- Support `--help` and `--version` as stable interfaces.
- Keep help output deterministic and friendly for copy/paste.
- Exit code conventions:
  - `0`: success/help/version output.
  - `2`: invalid CLI arguments.
  - Non-zero others: runtime/startup failures.
- Unknown options must fail fast with a clear error message.

## 3. Startup Output

- Keep startup banner concise.
- Version detail should come from `--version`, not verbose default startup logs.
- Runtime logs should be structured and module-tagged.

## 4. Compatibility Strategy

- Keep backward compatibility for one positional `config_path` argument.
- New options should be additive and avoid breaking scripts.

## 5. Documentation Discipline

- README should explicitly state:
  - Current implemented scope.
  - Non-goals / not-yet-implemented parts.
  - Extensibility points.
- New user-facing options must be documented in README and `--help`.

## 6. Extensibility Architecture

- Use interface-driven modules for external systems:
  - Database pools.
  - Storage providers.
  - Outbound delivery and DNS routing.
- New providers should integrate via factory/config without touching FSM core logic.

## 7. Build and Reproducibility

- All generated files belong to the build directory.
- Source tree should only keep scripts and templates.
- Build script should auto-heal stale CMake cache/source mismatch.

## 8. Logging and Observability

- Default release log level: info.
- Keep debug-level logs behind compile-time switches.
- Include request identifiers (message ID/mail ID) where possible.

## 9. Security Baseline

- Do not commit secrets (DB password/private keys).
- Prefer mounted runtime secrets/config for deployment.
- Enforce cert/key file existence checks during startup.
- Project must include a `LICENSE` file with the chosen open-source license.

## 10. Test and Change Quality

- For new features, include at least one runtime verification command.
- Keep changes small and focused; avoid unrelated formatting-only edits.
