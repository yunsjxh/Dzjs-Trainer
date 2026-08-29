#pragma once

#include "Logger.h"

namespace GuardTerminator {

// Runs remediation. A signaled stopEvent cancels the respawn wait.
bool Execute(Logger* logger, HANDLE stopEvent = nullptr);

// Dry-run mode only enumerates and validates targets.
bool DryRun(Logger* logger);

}  // namespace GuardTerminator
