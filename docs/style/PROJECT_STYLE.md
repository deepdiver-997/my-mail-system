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

## 11. Async / Threading Model

### 11.1 改写 io 对象必须 post 到 io_context

- **事件循环（`io_context::run()` / `post()` / `dispatch()`）线程安全；单个 I/O 对象
  （`steady_timer` / `socket` / `ssl::stream`）不线程安全。** 同一个对象不能在多线程
  并发调用非 const 方法（`expires_after` / `cancel` / `async_read_some` / `async_write`）。
- 常见的误解："`async_*` 会 rebind 回 io_context，所以跨线程安全"。这只保证**完成回调
  在 io_context 线程执行**，并不保证从别的线程改这个对象安全——对象内部状态（如 timer
  接入的 io_context 共享 timer 堆）在 io 线程 dispatch 时会并发触碰，是真实竞争、可致 UB。
- **规则：凡可能被 io_context 所在线程触碰的 io 对象，要改它就 `boost::asio::post(exec,
  lambda)` marshal 到 io_context 任务队列串行执行**——多一次 post-fetch 任务的开销，
  换来与 io 线程 dispatch 之间的串行化。
  - 例：`SessionBase::rearm()` / `disarm_timeout()` 内部 `post` 到 `connection_->get_executor()`
    再改 timer（异步超时回收 watchdog）。
  - 例：远程存储装饰器把阻塞/耗时 op `post` 到 worker 池避免卡 io；回调再续。

### 11.2 两条都成立的正规异步续跑路径（别误判违规）

1. **回调 `post` 回 io 线程**再碰 io 对象（即 11.1 的规则）。
2. **回调在 worker 线程续跑**，靠调用方先 `set_paused(true)` 取得 session 独占
   （SPF/DNS/commit 回调约定；S3 装饰器即此类）。

选哪条：续跑里**要立刻改 io 对象**就 post 回 io；只要续跑解析内存状态、不改 io 对象，
可在 worker 线程续。

> 心理学捷径：读路径用 `set_paused` 让 worker 与 io 互斥，timer 这类没有 pause 可借的
> 对象用 `post` 把操作挪到同线程——两条都是正确隔离，只是手段不同。
