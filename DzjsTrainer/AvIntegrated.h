#pragma once

#include "stdafx.h"

#define AV_INTEGRATED_MAX_FINDINGS 8
#define AV_INTEGRATED_FINDING_CHARS 640

typedef struct _AV_PROCESS_SCAN_SUMMARY {
	DWORD processCount;
	DWORD scannedCount;
	DWORD detectionCount;
	DWORD failedCount;
	BOOL cancelled;
	DWORD findingCount;
	WCHAR findings[AV_INTEGRATED_MAX_FINDINGS][AV_INTEGRATED_FINDING_CHARS];
} AV_PROCESS_SCAN_SUMMARY, *PAV_PROCESS_SCAN_SUMMARY;

BOOL AvIntegratedEnsureLoaded(LPCWSTR driverPath);
// Stops and removes an older AV kernel service before replacing its image.
BOOL AvIntegratedPrepareForReplacement();
// Authorizes and stops the AV kernel driver while keeping its service reusable.
BOOL AvIntegratedUnload();
// Arms a still-running previous-day/legacy AV driver for unload (its service
// name no longer matches today's generated name). No-op when the running
// instance is today's service.
void AvIntegratedArmLegacyDriverForUnload();
BOOL AvIntegratedIsLoaded();
BOOL AvIntegratedSetProtectedProcess(DWORD processId);
BOOL AvIntegratedScanFile(LPCWSTR path, LPWSTR matchedName, DWORD matchedNameCount, BOOL* detected);
BOOL AvIntegratedScanAllProcessMemory(HANDLE cancelEvent, PAV_PROCESS_SCAN_SUMMARY summary);
BOOL AvIntegratedAddExePatternSignature(
	LPCWSTR const* paths,
	DWORD pathCount,
	LPWSTR patternText,
	DWORD patternTextCount,
	ULONG* signatureId,
	DWORD* exactByteCount,
	BOOL* packedSample);
BOOL AvIntegratedAddProcessPatternSignature(
	DWORD processId,
	LPWSTR patternText,
	DWORD patternTextCount,
	ULONG* signatureId,
	DWORD* exactByteCount);
BOOL AvIntegratedAddPatternSignatureText(
	LPCWSTR patternText,
	LPCWSTR signatureName,
	ULONG* signatureId,
	DWORD* exactByteCount);
BOOL AvIntegratedAddPatternSignatureTextBatch(
	LPCWSTR patternText,
	LPCWSTR signatureName,
	ULONG* lastSignatureId,
	DWORD* registeredCount,
	DWORD* exactByteCount);
void AvIntegratedClose();
