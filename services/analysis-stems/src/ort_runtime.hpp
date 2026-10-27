// analysis-stems: INNRuntime backed by ONNX Runtime, loaded dynamically at run time.
// Nothing of ONNX Runtime is linked: only its MIT C API header is compiled in. If the shared
// library is missing or unusable, the runtime reports "unavailable" and loadModel() fails with
// SQ_E_UNSUPPORTED.
#pragma once

#include <string>

namespace sq {
struct INNRuntime;
}

namespace sq::stems {

// Library file name for this platform ("onnxruntime.dll", "libonnxruntime.so", ...).
const char* ortLibraryName();

// Finds an ONNX Runtime shared library: explicit path (setting/env) first, then next to the
// executable and in its services/ folder. Empty if none exists.
std::string findOrtLibrary(const std::string& explicitPath);

// New runtime object (refcount 1) that lazily opens `libraryPath` on first use.
INNRuntime* createOrtRuntime(const std::string& libraryPath);

}  // namespace sq::stems
