#if defined(ECS_USE_MODULES)
import ecs;
#else
#if 1 // Use this for dev
#include "runtime.h"
#else
// Use the single-include header, it's faster to compile
#include "ecs_sh.h"
#endif
#endif
