// Share the production GPU harness while keeping the focused RT target
// independent of unrelated comparison test registrations.
#define KYTY_RAY_TRACING_TESTS_ONLY 1
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "ShaderRecompilerComputeTests.cpp"
