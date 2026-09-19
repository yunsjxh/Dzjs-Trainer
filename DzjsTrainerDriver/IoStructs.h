#pragma once

#define JDRV_PROTOCOL_MAGIC 0x5652444AUL
#define JDRV_PROTOCOL_VERSION 3UL
#define JDRV_ARCHITECTURE_X64 64UL

#define JDRV_CAPABILITY_TERMINATE_PROCESS 0x00000001UL
#define JDRV_CAPABILITY_SELF_PROTECT 0x00000002UL
#define JDRV_CAPABILITY_SUSPEND_PROCESS 0x00000004UL
#define JDRV_CAPABILITY_POWER_CONTROL 0x00000008UL
#define JDRV_CAPABILITY_REGISTRY_PROTECTION 0x00000010UL
#define JDRV_CAPABILITY_TOKEN_AUTHORIZED_UNLOAD 0x00000020UL
#define JDRV_CAPABILITY_IMAGE_LOAD_NOTIFY 0x00000040UL
#define JDRV_CAPABILITY_THREAD_NOTIFY 0x00000080UL
#define JDRV_CAPABILITY_EVENT_STREAM 0x00000100UL
#define JDRV_CAPABILITY_HEARTBEAT 0x00000200UL
#define JDRV_CAPABILITY_DRIVER_GUARD 0x00000400UL

#define JDRV_EVENT_VERSION 1UL
#define JDRV_EVENT_TYPE_IMAGE_LOAD 1UL
#define JDRV_EVENT_TYPE_THREAD_CREATE 2UL
#define JDRV_EVENT_TYPE_THREAD_EXIT 3UL
#define JDRV_EVENT_TYPE_DRIVER_BLOCKED 4UL
#define JDRV_EVENT_FLAG_SYSTEM_IMAGE 0x00000001UL
#define JDRV_EVENT_FLAG_PROTECTED_TARGET 0x00000002UL
#define JDRV_EVENT_FLAG_DRIVER_GUARD 0x00000004UL
#define JDRV_EVENT_IMAGE_PATH_CHARS 260UL
#define JDRV_EVENT_BATCH_CAPACITY 16UL

#define JDRV_GUARD_BLOCK_REASON_NOT_WHITELISTED 1UL
#define JDRV_GUARD_BLOCK_REASON_HASH_MISMATCH 2UL
#define JDRV_GUARD_BLOCK_REASON_PATCH_FAILED 3UL

#define JDRV_GUARD_NAME_CHARS 64UL
#define JDRV_GUARD_HASH_SIZE 32UL
#define JDRV_GUARD_ENTRY_FLAG_HASH_VALID 0x00000001UL

#define JDRV_GUARD_ACTION_CLEAR 1UL
#define JDRV_GUARD_ACTION_ADD 2UL
#define JDRV_GUARD_ACTION_ARM 3UL
#define JDRV_GUARD_ACTION_DISARM 4UL

#define JDRV_GUARD_MAX_ENTRIES_PER_REQUEST 64UL
#define JDRV_GUARD_MAX_WHITELIST_ENTRIES 4096UL

#define JDRV_UNLOAD_TOKEN_MAGIC 0x4B4F4C55UL
#define JDRV_UNLOAD_TOKEN_VERSION 1UL
#define JDRV_UNLOAD_TOKEN_ACTION_UNLOAD 1UL
#define JDRV_UNLOAD_TOKEN_MACHINE_HASH_SIZE 32UL
#define JDRV_UNLOAD_TOKEN_NONCE_SIZE 16UL
#define JDRV_UNLOAD_TOKEN_SIGNATURE_SIZE 64UL

#define JDRV_INIT_FLAG_WINDOWS_XP 0x00000001UL
#define JDRV_INIT_FLAG_WINDOWS_7_OR_LATER 0x00000002UL

typedef struct tag_JDRV_VERSION_RESPONSE {
    unsigned long size;
    unsigned long magic;
    unsigned long version;
    unsigned long architecture;
    unsigned long capabilities;
} JDRV_VERSION_RESPONSE, *PJDRV_VERSION_RESPONSE;

typedef struct tag_JDRV_INITPARAM {
    unsigned long size;
    unsigned long version;
    unsigned long flags;
    unsigned long systemVersion;
} JDRV_INITPARAM, *PJDRV_INITPARAM;

typedef struct tag_JDRV_HEARTBEAT_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long processId;
    unsigned long sequence;
} JDRV_HEARTBEAT_REQUEST, *PJDRV_HEARTBEAT_REQUEST;

typedef struct tag_JDRV_HEARTBEAT_RESPONSE {
    unsigned long size;
    unsigned long version;
    unsigned long accepted;
    unsigned long reserved;
    unsigned long processId;
    unsigned long protocolVersion;
    unsigned __int64 sequence;
    unsigned __int64 age100ns;
} JDRV_HEARTBEAT_RESPONSE, *PJDRV_HEARTBEAT_RESPONSE;

typedef struct tag_JDRV_PROCESS_REQUEST {
    unsigned long size;
    unsigned long version;
    unsigned long processId;
    unsigned long reserved;
} JDRV_PROCESS_REQUEST, *PJDRV_PROCESS_REQUEST;

typedef struct tag_JDRV_OPERATION_RESPONSE {
    unsigned long size;
    long status;
} JDRV_OPERATION_RESPONSE, *PJDRV_OPERATION_RESPONSE;

#pragma pack(push, 8)
typedef struct tag_JDRV_EVENT_RECORD {
    unsigned long size;
    unsigned long version;
    unsigned long type;
    unsigned long flags;
    unsigned __int64 sequence;
    unsigned __int64 timestamp;
    unsigned long processId;
    unsigned long threadId;
    unsigned __int64 imageBase;
    unsigned __int64 imageSize;
    unsigned long imagePathLength;
    unsigned long reserved;
    unsigned short imagePath[JDRV_EVENT_IMAGE_PATH_CHARS];
} JDRV_EVENT_RECORD, *PJDRV_EVENT_RECORD;

typedef struct tag_JDRV_EVENT_BATCH {
    unsigned long size;
    unsigned long version;
    unsigned long eventCount;
    unsigned long droppedEvents;
    JDRV_EVENT_RECORD events[JDRV_EVENT_BATCH_CAPACITY];
} JDRV_EVENT_BATCH, *PJDRV_EVENT_BATCH;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tag_JDRV_UNLOAD_TOKEN {
    unsigned long size;
    unsigned long magic;
    unsigned long version;
    unsigned long action;
    unsigned char machineHash[JDRV_UNLOAD_TOKEN_MACHINE_HASH_SIZE];
    unsigned __int64 notBeforeFileTime;
    unsigned __int64 notAfterFileTime;
    unsigned char nonce[JDRV_UNLOAD_TOKEN_NONCE_SIZE];
    unsigned char signature[JDRV_UNLOAD_TOKEN_SIGNATURE_SIZE];
} JDRV_UNLOAD_TOKEN, *PJDRV_UNLOAD_TOKEN;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct tag_JDRV_GUARD_ENTRY {
    unsigned long size;
    unsigned long flags;
    unsigned __int64 fileSize;
    unsigned short name[JDRV_GUARD_NAME_CHARS];
    unsigned char sha256[JDRV_GUARD_HASH_SIZE];
} JDRV_GUARD_ENTRY, *PJDRV_GUARD_ENTRY;

typedef struct tag_JDRV_GUARD_CONFIG {
    unsigned long size;
    unsigned long version;
    unsigned long action;
    unsigned long entryCount;
    unsigned long reserved;
    JDRV_GUARD_ENTRY entries[1];
} JDRV_GUARD_CONFIG, *PJDRV_GUARD_CONFIG;

typedef struct tag_JDRV_GUARD_STATUS {
    unsigned long size;
    unsigned long version;
    unsigned long armed;
    unsigned long entryCount;
    unsigned long long evaluatedImages;
    unsigned long long allowedImages;
    unsigned long long blockedImages;
    unsigned long long patchFailures;
} JDRV_GUARD_STATUS, *PJDRV_GUARD_STATUS;
#pragma pack(pop)
