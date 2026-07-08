// lazily_ffi — C-ABI boundary (placeholder, populated by the FFI layer).
#include <lazily/lazily.hpp>

extern "C" {

int lazily_protocol_version(void) { return ::lazily::kProtocolVersion; }

}  // extern "C"
