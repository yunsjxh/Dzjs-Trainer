#pragma once

#include "stdafx.h"

// Defensive demo: baseline and continuously score suspicious process/image paths.
// The demo is audit-only and never terminates, suspends, or modifies a process.
BOOL ActiveDefenseDemoStart();
void ActiveDefenseDemoStop();
BOOL ActiveDefenseDemoScoreImage(ULONG processId, LPCWSTR imagePath, BOOL systemImage, BOOL protectedTarget);
