#pragma once

namespace flexedge::node::v2 {
class SecurityPolicy;
}
namespace flexedge::node {
void validate_security(const v2::SecurityPolicy& policy);
}
