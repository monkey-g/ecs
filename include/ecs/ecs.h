#if defined(ECS_USE_MODULES)
import ecs;
#else
#if 1 // Use this for dev
#ifndef ECS_EXPORT
#define ECS_EXPORT
#endif
#include "runtime.h"
#else
// Use the single-include header, it's faster to compile
#include "ecs_sh.h"
#endif
#endif
