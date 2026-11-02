// Paint Engine - OpenGL ES backend common includes and helpers.
//
// Baseline: OpenGL ES 3.0 (linked through libGLESv3). Anything newer (3.1/3.2
// or extensions) is detected at runtime and called through function pointers
// obtained from eglGetProcAddress, never linked directly.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>  // constants only for 3.1/3.2; functions are not called directly
#include <GLES2/gl2ext.h>

#include <string>

namespace pe::gles {

const char* eglErrorString(EGLint error);
const char* glErrorString(GLenum error);

// Drains glGetError(); logs every pending error with `where`. Returns the
// number of errors found.
int drainGLErrors(const char* where);

// Optional entry points (null when unsupported).
struct GLExtFunctions {
    PFNGLDEBUGMESSAGECALLBACKKHRPROC debugMessageCallback = nullptr;
    PFNGLPUSHDEBUGGROUPKHRPROC pushDebugGroup = nullptr;
    PFNGLPOPDEBUGGROUPKHRPROC popDebugGroup = nullptr;
    PFNGLOBJECTLABELKHRPROC objectLabel = nullptr;
    // GL_KHR_robustness / GL_EXT_robustness (KHR and EXT share the signature).
    PFNGLGETGRAPHICSRESETSTATUSKHRPROC getGraphicsResetStatus = nullptr;
};

}  // namespace pe::gles
