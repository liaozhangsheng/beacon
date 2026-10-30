#pragma once

#include <cstddef>

struct ssl_ctx_st;

namespace beacon::http {

// Adds the operating system's trusted root certificates to an OpenSSL context.
// A bundled OpenSSL only knows the certificate paths of the machine that built
// it, so its default verify paths are not enough on end-user machines.
// Returns the number of certificates in the context's store afterwards.
std::size_t add_system_trust(ssl_ctx_st* context);

}  // namespace beacon::http
