# Framework Extraction Refactor (2026-07)

## Summary

Extracted a reusable server framework from the mail-system codebase. The framework is transport-agnostic (no TCP/SSL dependency in `ServerBase`), protocol-agnostic (no SMTP/IMAP knowledge in `SessionBase` or `FsmBase`), and lives in `include/framework/`.

## Before / After

### Class Hierarchy

**Before (3-layer dual inheritance):**
```
SmtpsFsm ──┐
            ├── TraditionalSmtpsFsm
FsmBase ────┘

ServerBase (held PersistentQueue, OutboundClient, acceptors, SSL context)
```

**After (flat single inheritance):**
```
FsmBase ── TraditionalSmtpsFsm  (all business methods moved here)

ServerBase (zero TCP/SSL, zero mail dependencies)
  └── TcpServerBase<SessionType>  (TCP/SSL accept logic)
        └── SmtpsServer  (persistent queue, outbound client)
        └── ImapsServer
```

### Handler Signature

**Before:** `void handler(shared_ptr<SessionBase>, const string& args)` — passed args redundantly
**After:** `void handler(shared_ptr<SessionBase>)` — reads `session->get_last_command_args()` when needed

### Directory Structure

**Before:**
```
include/mail_system/back/mailServer/
  session/session_base.h      (282 lines, mixed decl+impl, held mail_/usr_)
  fsm/ (no common base)
  server_base.h               (coupled to TCP, persistent queue, outbound)
```

**After:**
```
include/framework/
  session_base.h              (122 lines, declarations only)
  session_base.tpp            (implementation)
  fsm_base.h                  (transition table + dispatch + hooks)
  server_base.h               (pure lifecycle/config/metrics)
  tcp_server_base.h           (TCP/SSL acceptors, listener thread)
  connection/                 (IConnection, TcpConnection, SslConnection)
  thread_pool/                (ThreadPoolBase, IOThreadPool, BoostThreadPool)
  metrics_server.h
  intrusion_detector.h
  server_config.h
```

## Key Changes

### 1. SessionBase de-mailification (commit `efaf1e4`)
- `mail_`, `usr_` members moved from `SessionBase` to `SmtpsSession`
- `SessionBase` no longer includes `entities/mail.h` or `entities/usr.h`
- `.h` split into declarations-only (122 lines) + `.tpp` implementation

### 2. FsmBase extraction (commit `db8a3b1`)
- Template: `FsmBase<ConnectionType, State, Event>`
- Provides `add_transition()`, `add_handler()`, `dispatch()`
- 5 virtual hooks: `is_terminal_state`, `on_invalid_transition`, `on_handler_not_found`, `pre_dispatch`, `invoke_handler`
- SMTP and IMAP `process_event` simplified from ~30 lines to a single `dispatch()` call

### 3. Args removal (commit `f6122f4`)
- Handler signature unified to `void(session)`
- Command data accessed via `session->get_last_command_args()`
- `ImapContext` gained `is_uid_command` / `uid_overridden_args` for UID subcommands

### 4. FSM layer merge (commits `db3af9c`, `e94d627`)
- `SmtpsFsm` class deleted, members moved to `TraditionalSmtpsFsm`
- `ImapsFsm` class deleted, members moved to `TraditionalImapsFsm`
- `*_fsm.hpp` now contains only type definitions (State, Event, Context enums/structs)
- Dual inheritance eliminated

### 5. ServerBase de-coupling (commit `7cf5e54`)
- `m_persistentQueue`, `m_outboundClient`, `m_outboundInterruptFlag` moved to `SmtpsServer`
- ServerBase no longer includes `persistent_queue.h` or `smtp_outbound_client.h`

### 6. TcpServerBase extraction (commit `7354ca6`)
- Template: `TcpServerBase<TcpSession, SslSession>`
- Holds io_context, SSL context, acceptors, listener thread
- Subclasses implement `make_tcp_session()` / `make_ssl_session()` factories
- ServerBase becomes fully transport-agnostic

### 7. Build optimization (commits `c0e6362`, `f8ac36e`)
- Explicit template instantiation via `_inst.cpp` files
- 5 template classes instantiated once for `TcpConnection`/`SslConnection`
- CMakeLists.txt: `setup_mail_target()` function eliminates 200+ lines of repetition (740 → 295 lines)

### 8. Framework directory extraction (commit `849ea80`)
- 14 headers + 3 source files moved to `include/framework/` and `src/framework/`
- 31 files updated with new include paths

## Design Decisions

### Why ServerBase is NOT a template
ServerBase depends only on `IConnection` interface, not on the transport type. Different transports (TCP, QUIC) have fundamentally different listener logic — acceptor vs. datagram socket vs. QUIC connection. Each transport deserves its own ServerBase subclass, not a template parameter.

### Why `do_handshake` stays in the header
`do_handshake<HandshakeHandler>` is a member template — each call site's lambda creates a unique instantiation. Member templates cannot be explicitly instantiated, so it remains inline in `session_base.h`.

### Error codes
`SessionError` enum on `SessionBase` with virtual `error_message()` mapping. FSM handlers set error via `session->set_error()`, protocol-specific subclasses override the mapping for tailored messages.

## Future Work

- Move `IOThreadPool` ownership from `ServerBase` to `TcpServerBase` (currently shared via `ThreadPoolBase*` cast)
- Outbound SMTP normalization: apply the same FsmBase + SessionBase pattern to outbound delivery
- QUIC transport: implement `QuicServerBase : ServerBase` with `IConnection`-compatible QUIC streams
