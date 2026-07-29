// Smoke test: verify OutboundSmtpSession + OutboundSmtpFsm compile
#include "mail_system/back/outbound/outbound_smtp_session.h"

int main() {
    using namespace mail_system;
    using namespace mail_system::outbound;

    auto fsm = std::make_shared<OutboundSmtpFsm<TcpConnection>>();
    (void)fsm;

    return 0;
}
