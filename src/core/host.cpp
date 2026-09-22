#include "wiinx/core/host.hpp"

namespace wiinx {
namespace {
Host g_host;
}  // namespace

void set_host(const Host& host) noexcept { g_host = host; }
const Host& host() noexcept { return g_host; }

}  // namespace wiinx
