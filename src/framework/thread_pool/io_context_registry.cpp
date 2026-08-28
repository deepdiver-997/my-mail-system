#include "framework/thread_pool/io_context_registry.h"

namespace mail_system {

namespace {
thread_local boost::asio::io_context* tls_current_ctx = nullptr;
} // namespace

boost::asio::io_context* current_io_context() {
    return tls_current_ctx;
}

void set_current_io_context(boost::asio::io_context* ctx) {
    tls_current_ctx = ctx;
}

} // namespace mail_system
