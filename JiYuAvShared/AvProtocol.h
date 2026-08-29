#pragma once

#ifdef _KERNEL_MODE
#include <ntifs.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define JIYU_AV_DEVICE_TYPE 0xA831UL
#define JIYU_AV_PROTOCOL_MAGIC 0x5641594AUL
#define JIYU_AV_PROTOCOL_VERSION 4UL

#define JIYU_AV_MAX_SIGNATURES 256UL
#define JIYU_AV_MAX_PATTERN_BYTES 64UL
#define JIYU_AV_MAX_SIGNATURE_NAME 64UL
#define JIYU_AV_MAX_PATH_CHARS 520UL
#define JIYU_AV_SHA256_BYTES 32UL
#define JIYU_AV_MAX_BUFFER_SCAN (1024UL * 1024UL)
#define JIYU_AV_PROCESS_SAMPLE_BYTES 64UL

#define JIYU_AV_SIGNATURE_SHA256 1UL
#define JIYU_AV_SIGNATURE_PATTERN 2UL

#define JIYU_AV_SCAN_FLAG_MATCHED 0x00000001UL
#define JIYU_AV_SCAN_FLAG_HASH_VALID 0x00000002UL

#define JIYU_AV_IOCTL_QUERY_VERSION \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x800UL, METHOD_BUFFERED, FILE_READ_ACCESS)
#define JIYU_AV_IOCTL_REGISTER_CONTROLLER \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x801UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_SET_PROTECTED_PROCESS \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x802UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_ADD_SIGNATURE \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x803UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_CLEAR_SIGNATURES \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x804UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_SCAN_BUFFER \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x805UL, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_SCAN_FILE \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x806UL, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_QUERY_STATS \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x807UL, METHOD_BUFFERED, FILE_READ_ACCESS)
#define JIYU_AV_IOCTL_ARM_UNLOAD \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x808UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_REMOVE_SIGNATURE \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x809UL, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_QUERY_SIGNATURE \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x80AUL, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_SCAN_PROCESS \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x80BUL, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JIYU_AV_IOCTL_SAMPLE_PROCESS_IMAGE \
    CTL_CODE(JIYU_AV_DEVICE_TYPE, 0x80CUL, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#pragma pack(push, 8)

typedef struct _JIYU_AV_VERSION_RESPONSE {
    unsigned long size;
    unsigned long magic;
    unsigned long version;
    unsigned long architecture;
    unsigned long maxSignatures;
    unsigned long maxPatternBytes;
} JIYU_AV_VERSION_RESPONSE, *PJIYU_AV_VERSION_RESPONSE;

typedef struct _JIYU_AV_PROCESS_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long processId;
    unsigned long reserved;
} JIYU_AV_PROCESS_REQUEST, *PJIYU_AV_PROCESS_REQUEST;

typedef struct _JIYU_AV_PROCESS_SAMPLE_RESPONSE {
    unsigned long size;
    long status;
    unsigned long processId;
    unsigned long dataLength;
    unsigned __int64 address;
    unsigned char data[JIYU_AV_PROCESS_SAMPLE_BYTES];
} JIYU_AV_PROCESS_SAMPLE_RESPONSE, *PJIYU_AV_PROCESS_SAMPLE_RESPONSE;

typedef struct _JIYU_AV_SIGNATURE_RECORD {
    unsigned long size;
    unsigned long version;
    unsigned long signatureId;
    unsigned long type;
    unsigned long dataLength;
    unsigned long reserved;
    wchar_t name[JIYU_AV_MAX_SIGNATURE_NAME];
    unsigned char data[JIYU_AV_MAX_PATTERN_BYTES];
    unsigned char mask[JIYU_AV_MAX_PATTERN_BYTES];
} JIYU_AV_SIGNATURE_RECORD, *PJIYU_AV_SIGNATURE_RECORD;

typedef struct _JIYU_AV_SIGNATURE_ID_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long signatureId;
    unsigned long reserved;
} JIYU_AV_SIGNATURE_ID_REQUEST, *PJIYU_AV_SIGNATURE_ID_REQUEST;

typedef struct _JIYU_AV_SIGNATURE_QUERY_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long index;
    unsigned long reserved;
} JIYU_AV_SIGNATURE_QUERY_REQUEST, *PJIYU_AV_SIGNATURE_QUERY_REQUEST;

typedef struct _JIYU_AV_SIGNATURE_QUERY_RESPONSE {
    unsigned long size;
    unsigned long version;
    unsigned long index;
    unsigned long totalCount;
    JIYU_AV_SIGNATURE_RECORD record;
} JIYU_AV_SIGNATURE_QUERY_RESPONSE, *PJIYU_AV_SIGNATURE_QUERY_RESPONSE;

typedef struct _JIYU_AV_SCAN_FILE_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long pathLength;
    unsigned long reserved;
    wchar_t path[JIYU_AV_MAX_PATH_CHARS];
} JIYU_AV_SCAN_FILE_REQUEST, *PJIYU_AV_SCAN_FILE_REQUEST;

typedef struct _JIYU_AV_SCAN_RESULT {
    unsigned long size;
    long status;
    unsigned long flags;
    unsigned long signatureId;
    unsigned long signatureType;
    unsigned long reserved;
    unsigned __int64 matchOffset;
    unsigned __int64 scannedBytes;
    unsigned char sha256[JIYU_AV_SHA256_BYTES];
    wchar_t signatureName[JIYU_AV_MAX_SIGNATURE_NAME];
} JIYU_AV_SCAN_RESULT, *PJIYU_AV_SCAN_RESULT;

typedef struct _JIYU_AV_STATS_RESPONSE {
    unsigned long size;
    unsigned long version;
    unsigned long signatureCount;
    unsigned long protectedProcessId;
    unsigned __int64 filesScanned;
    unsigned __int64 buffersScanned;
    unsigned __int64 detections;
    unsigned __int64 bytesScanned;
} JIYU_AV_STATS_RESPONSE, *PJIYU_AV_STATS_RESPONSE;

#pragma pack(pop)
