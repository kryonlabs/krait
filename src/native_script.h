#ifndef KRAIT_NATIVE_SCRIPT_H
#define KRAIT_NATIVE_SCRIPT_H

/* Scene-authored scripting (see src/native_script.c). Runs per frame
 * while playing; variables persist per node and reset on Play. */
#include "native_engine_internal.h"

void krait_script_run(EngineNode *node, float dt);

#endif /* KRAIT_NATIVE_SCRIPT_H */
