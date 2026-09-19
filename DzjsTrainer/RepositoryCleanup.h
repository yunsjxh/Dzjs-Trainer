#pragma once

#include "Logger.h"

namespace RepositoryCleanup {

// Runs the repository-style cleanup independently of GuardTerminator.
bool Execute(Logger* logger, HANDLE stopEvent = nullptr);

}  // namespace RepositoryCleanup
