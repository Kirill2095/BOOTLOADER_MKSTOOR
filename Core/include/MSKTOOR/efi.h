/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — minimal self-contained UEFI definitions.
 *
 * We deliberately do NOT depend on gnu-efi or EDK2 headers: the bootloader
 * carries exactly the structures it touches, every field sized explicitly.
 * Reference: UEFI Specification 2.10, sections 2, 4, 6, 7, 13.
 */
#ifndef MKSTOOR_EFI_H
#define MKSTOOR_EFI_H

#include <mkstoor/types.h>

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_INPUT_TEXT; /* fwd */

#define EFI_SUCCESS               0ULL
#define EFI_ERROR(s)              (((i64)(s)) < 0)
#define EFI_LOAD_ERROR            0x8000000000000001ULL
#define EFI_INVALID_PARAMETER     0x8000000000000002ULL
#define EFI_UNSUPPORTED           0x8000000000000003ULL
#define EFI_BAD_BUFFER_SIZE       0x8000000000000004ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_READY             0x8000000000000006ULL
#define EFI_DEVICE_ERROR          0x8000000000000007ULL
#define EFI_WRITE_PROTECTED       0x8000000000000008ULL
#define EFI_OUT_OF_RESOURCES      0x8000000000000009ULL
#define EFI_NOT_FOUND             0x800000000000000EULL
#define EFI_ACCESS_DENIED         0x800000000000000FULL
#define EFI_MEDIA_CHANGED         0x800000000000000BULL
#define EFI_NO_MEDIA              0x8000000000000013ULL

/* ------------------------------------------------------------------------ */
/* GUID                                                                      */
/* ------------------------------------------------------------------------ */
typedef struct {
        u32 Data1;
        u16 Data2;
        u16 Data3;
        u8  Data4[8];
} EFI_GUID;

static inline bool guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
        return a->Data1 == b->Data1 && a->Data2 == b->Data2 &&
               a->Data3 == b->Data3 &&
               *(const u64 *)a->Data4 == *(const u64 *)b->Data4;
}

#define MKS_GUID(data1, data2, data3, d0, d1, d2, d3, d4, d5, d6, d7) \
        { data1, data2, data3, { d0, d1, d2, d3, d4, d5, d6, d7 } }

/* Well-known GUIDs we consume. */
static const EFI_GUID mks_global_variable =
        MKS_GUID(0x8be4df61, 0x93ca, 0x11d3,
               0x87, 0xb0, 0x64, 0x85, 0xd2, 0xe9, 0x5c, 0x5d);
static const EFI_GUID mks_file_system_info_id =
        MKS_GUID(0x096e1921, 0xc93b, 0x43db,
               0xb7, 0x1b, 0xcd, 0xb0, 0x16, 0x2c, 0x0c, 0x50);
/* Stage 3: graphics & pointing devices. */
static const EFI_GUID mks_gop_guid =
        MKS_GUID(0x9042a9de, 0x23dc, 0x4a38,
               0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a);
static const EFI_GUID mks_simple_pointer_guid =
        MKS_GUID(0x31878c87, 0xb11, 0x45f0,
               0x89, 0x6f, 0xca, 0x75, 0x27, 0xb6, 0xc1, 0x4c);
static const EFI_GUID mks_device_path_guid =
        MKS_GUID(0x09576e91, 0x6d3f, 0x11d2,
               0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);

/* ------------------------------------------------------------------------ */
/* CHAR16 helpers                                                            */
/* ------------------------------------------------------------------------ */
typedef u16 CHAR16;

static inline usize efi_strnlen(const CHAR16 *s, usize max)
{
        usize n = 0;
        while (n < max && s[n] != 0)
                n++;
        return n;
}

/* ASCII C string -> UTF-16LE buffer. Returns chars written (excl. NUL). */
static inline usize efi_str_to_16(const char *ascii, CHAR16 *out, usize cap)
{
        usize i = 0;
        for (; ascii[i] && i + 1 < cap; i++)
                out[i] = (CHAR16)(u8)ascii[i];
        out[i] = 0;
        return i;
}

/* UTF-16LE -> ASCII (best effort: non-Latin-1 codepoints become '?').
 * Always NUL-terminates when cap > 0. Returns chars written (excl. NUL). */
static inline usize efi_str_from_16(const CHAR16 *src, char *dst, usize cap)
{
        usize i = 0;
        if (!cap)
                return 0;
        for (; src[i] && i + 1 < cap; i++) {
                u16 c = src[i];
                dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
        }
        dst[i] = '\0';
        return i;
}

/* ------------------------------------------------------------------------ */
/* Memory map                                                                */
/* ------------------------------------------------------------------------ */
typedef enum {
        EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData,
        EfiBootServicesCode,   EfiBootServicesData,
        EfiRuntimeServicesCode, EfiRuntimeServicesData,
        EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
        EfiACPIMemoryNVS,      EfiMemoryMappedIO,
        EfiMemoryMappedIOPortSpace, EfiPalCode, EfiPersistentMemory,
        EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
        u32 Type;
        u64 PhysicalStart;
        u64 VirtualStart;
        u64 NumberOfPages;
        u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ------------------------------------------------------------------------ */
/* Simple Text Output                                                        */
/* ------------------------------------------------------------------------ */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
        EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                            BOOLEAN ExtendedVerification);
        EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                   CHAR16 *String);
        EFI_STATUS (*TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                 CHAR16 *String);
        EFI_STATUS (*QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                usize Mode);
        EFI_STATUS (*SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                              usize Mode);
        EFI_STATUS (*SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                   usize Attribute);
        EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
        EFI_STATUS (*SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                        usize Column, usize Row);
        EFI_STATUS (*EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                   BOOLEAN Visible);
        void *Mode; /* TEXT_OUTPUT_MODE — opaque, never dereferenced.
                     * Trailing fields must exist: the firmware's table is
                     * fixed-layout and our function pointers below this
                     * point would be misread without them. */
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL SIMPLE_TEXT_OUTPUT_INTERFACE;

/* BOOLEAN per EFI 2.10 (1 byte). */
#ifndef TRUE
typedef unsigned char BOOLEAN;
#define TRUE  1
#define FALSE 0
#endif

/* ------------------------------------------------------------------------ */
/* Simple Text Input                                                         */
/* ------------------------------------------------------------------------ */
typedef struct {
        CHAR16 WideChar;
        CHAR16 ScanCode;
} EFI_INPUT_KEY;

/* UEFI spec 2.10, Table "EFI_INPUT_KEY Scan Codes". Only the ones we use.
 * Note: Enter/Escape arrive as WideChar (L'\r'/L'\x1b') on most firmware;
 * the scan codes below are what ReadKeyStroke reports for arrows etc. */
#define SCAN_NULL       0x0000
#define SCAN_UP         0x0018
#define SCAN_DOWN       0x0019
#define SCAN_RIGHT      0x001A
#define SCAN_LEFT       0x001B
#define SCAN_HOME       0x001C
#define SCAN_END        0x001D
#define SCAN_ESC        0x0017
#define SCAN_INSERT     0x001E
#define SCAN_DELETE     0x001F
#define SCAN_PG_UP      0x0020
#define SCAN_PG_DOWN    0x0021

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
        EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                            BOOLEAN ExtendedVerification);
        EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
                                    EFI_INPUT_KEY *Key);
        void *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ------------------------------------------------------------------------ */
/* Block I/O                                                                 */
/* ------------------------------------------------------------------------ */
typedef struct {
        u32 MediaId;
        BOOLEAN RemovableMedia;
        BOOLEAN MediaPresent;
        BOOLEAN LogicalPartition;
        BOOLEAN ReadOnly;
        BOOLEAN WriteCaching;
        u32 PartitionRank;
        u64 PhysicalSize;
        u64 LogicalBlockSize;
        u64 LastBlockLba;
} EFI_BLOCK_IO_MEDIA_INFO;

typedef struct EFI_BLOCK_IO_PROTOCOL {
        void                 *Revision;
        EFI_BLOCK_IO_MEDIA_INFO *Media;
        EFI_STATUS (*Reset)(struct EFI_BLOCK_IO_PROTOCOL *This,
                            BOOLEAN ExtendedVerification);
        EFI_STATUS (*ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This,
                                 u32 MediaId, u64 LBA,
                                 usize BufferSize, void *Buffer);
        EFI_STATUS (*WriteBlocks)(struct EFI_BLOCK_IO_PROTOCOL *This,
                                  u32 MediaId, u64 LBA,
                                  usize BufferSize, const void *Buffer);
        EFI_STATUS (*Flush)(struct EFI_BLOCK_IO_PROTOCOL *This);
} EFI_BLOCK_IO_PROTOCOL;

/* ------------------------------------------------------------------------ */
/* File system protocols                                                     */
/* ------------------------------------------------------------------------ */
#define EFI_FILE_READ_ONLY      0x00000001u
#define EFI_FILE_HIDDEN         0x00000002u
#define EFI_FILE_SYSTEM         0x00000004u
#define EFI_FILE_DIRECTORY      0x00000010u
#define EFI_FILE_ARCHIVE        0x00000020u

typedef struct {
        u64 Size;
        CHAR16 FileName[256];
        u8 Attributes;
        /* Timestamps etc. omitted: we only need Size + name + attributes. */
} EFI_FILE_INFO;

typedef struct EFI_FILE_PROTOCOL {
        u64 Revision;
        EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL *This,
                           struct EFI_FILE_PROTOCOL **NewHandle,
                           CHAR16 *FileName, u64 OpenMode, u64 Attributes);
        EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *This);
        EFI_STATUS (*Delete)(struct EFI_FILE_PROTOCOL *This);
        EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *This,
                           usize *BufferSize, void *Buffer);
        EFI_STATUS (*Write)(struct EFI_FILE_PROTOCOL *This,
                            usize *BufferSize, const void *Buffer);
        EFI_STATUS (*GetPosition)(struct EFI_FILE_PROTOCOL *This, u64 *Position);
        EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *This, u64 Position);
        EFI_STATUS (*GetInfo)(struct EFI_FILE_PROTOCOL *This,
                              EFI_GUID *InformationType,
                              usize *BufferSize, void *Buffer);
        EFI_STATUS (*SetInfo)(struct EFI_FILE_PROTOCOL *This,
                              EFI_GUID *InformationType,
                              usize BufferSize, const void *Buffer);
        EFI_STATUS (*Flush)(struct EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
        u64 Revision;
        EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                 EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ------------------------------------------------------------------------ */
/* Graphics Output Protocol (UEFI 2.10 §12.9) — Stage 3                    */
/* ------------------------------------------------------------------------ */
typedef enum {
        PixelRedGreenBlueReserved8,
        PixelBlueGreenRedReserved8,
        PixelBitMask,
        PixelBltOnly,
        PixelFormatMax,
        PixelRGBA = 0x1000000, /* not a firmware format: our canonical one */
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
        u32 RedMask;
        u32 GreenMask;
        u32 BlueMask;
        u32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
        u32 Version;
        u32 HorizontalResolution;
        u32 VerticalResolution;
        EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
        EFI_PIXEL_BITMASK PixelInformation;
        u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
        u32 MaxMode;
        u32 Mode;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
        usize SizeOfInfo;
        u64 FrameBufferBase;
        u32 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* Blt operation codes. */
#define EFI_GRAPHICS_OUTPUT_BLT_PIXEL      0u
#define EFI_GRAPHICS_OUTPUT_BLT_VIDEO_FILL 1u
#define EFI_GRAPHICS_OUTPUT_BLT_TO_VIDEO   2u
#define EFI_GRAPHICS_OUTPUT_BLT_FROM_VIDEO 3u
#define EFI_GRAPHICS_OUTPUT_BLT_VIDEO_TO_BLT_COUNT 4u

typedef struct {
        union {
                struct { u8 Blue, Green, Red, Reserved; };
                u32 U32;
        };
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
        EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                                u32 ModeNumber, usize *SizeOfInfo,
                                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
        EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                              u32 ModeNumber);
        EFI_STATUS (*Blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                          u32 BltOp, usize SourceX, usize SourceY,
                          usize DestinationX, usize DestinationY,
                          usize Width, usize Height, usize DeltaBytes);
        EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ------------------------------------------------------------------------ */
/* Simple Pointer / Absolute Pointer (UEFI 2.10 §13) — Stage 3              */
/* ------------------------------------------------------------------------ */
typedef struct {
        i64 DeltaX;
        i64 DeltaY;
        i64 DeltaZ;
        u32 RightButton : 1;
        u32 LeftButton  : 1;
        u32 Active      : 1;
        u32 Reserved    : 29;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
        u32 ResolutionX;
        u32 ResolutionY;
        u32 ResolutionZ;
        BOOLEAN LeftButton;
        BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_MODE;

typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
        EFI_STATUS (*Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *This,
                            BOOLEAN ExtendedVerification);
        EFI_STATUS (*GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *This,
                               EFI_SIMPLE_POINTER_STATE *State);
        void *WaitForInput;
        EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;

typedef struct {
        u32 X;
        u32 Y;
} EFI_ABSOLUTE_POINTER_STATE_S; /* minimal: we only need X/Y */

/* ------------------------------------------------------------------------ */
/* Device paths                                                              */
/* ------------------------------------------------------------------------ */
typedef struct {
        u8 Type;
        u8 SubType;
        u8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

/* ------------------------------------------------------------------------ */
/* Boot Services                                                             */
/* ------------------------------------------------------------------------ */

/* For clarity we declare the full-order boot services table with the exact
 * UEFI 2.10 layout up to the functions MKSTOOR uses. Field order matters. */
typedef struct {
        u64 Signature;
        u32 Revision;
} EFI_TABLE_HEADER;

#define EFI_SYSTEM_TABLE_SIGNATURE     0x545359532049424FULL /* "SYSV_0x54535953..." little-endian "IBS SYSTEM" */
#define EFI_BOOT_SERVICES_SIGNATURE    0x565245132C9B1F9FULL
#define EFI_SYSTEM_TABLE_REVISION(maj, min) (((maj) << 16) | (min))

typedef struct EFI_BOOT_SERVICES_FULL {
        EFI_TABLE_HEADER Hdr;
        /* 1. RaiseTPL */
        EFI_STATUS (*RaiseTPL)(u32 NewTpl);
        /* 2. RestoreTPL */
        u32 (*RestoreTPL)(u32 OldTpl);
        /* 3-8 */
        EFI_STATUS (*AllocatePages)(u32 Type, EFI_MEMORY_TYPE MemType,
                                    u64 Pages, u64 *Memory);
        EFI_STATUS (*FreePages)(u64 Memory, u64 Pages);
        EFI_STATUS (*GetMemoryMap)(usize *MemoryMapSize,
                                   EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                   usize *MapKey, usize *DescriptorSize,
                                   u32 *DescriptorVersion);
        EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, usize Size,
                                   void **Buffer);
        EFI_STATUS (*FreePool)(void *Buffer);
        /* 9-16 */
        EFI_STATUS (*CreateEvent)(u32 Type, u32 NotifyTpl,
                                  void *NotifyFunction, void *NotifyContext,
                                  void **Event);
        EFI_STATUS (*SetTimer)(void *Event, u32 Type, u64 TriggerTime);
        EFI_STATUS (*WaitForEvent)(usize NoEvents, void **Event, usize *Index);
        EFI_STATUS (*SignalEvent)(void *Event);
        EFI_STATUS (*CloseEvent)(void *Event);
        EFI_STATUS (*CheckEvent)(void *Event);
        /* 17-20: install/prot handlers (unused, keep slot) */
        void *InstallProtocolHandler[4];
        /* 21 */
        EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, const EFI_GUID *Protocol,
                                     void **Interface);
        /* 22. Image handle of the running application (Stage 3 pointer
         * mapping reads it to find the device path of ConsoleOut). */
        EFI_HANDLE ImageHandle;
        /* 23-24 search */
        EFI_STATUS (*LocateHandle)(u32 SearchType, const EFI_GUID *ProtocolType,
                                   EFI_HANDLE SearchKey, usize *BufferSize,
                                   EFI_HANDLE *Buffer);
        EFI_STATUS (*LocateProtocol)(const EFI_GUID *Protocol,
                                     EFI_GUID *Registration, void **Interface);
        /* 25-28 load image family */
        EFI_STATUS (*LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentHandle,
                                 void *DevicePath, void *SourceBuffer,
                                 usize SourceSize, EFI_HANDLE *NewHandle);
        EFI_STATUS (*StartImage)(EFI_HANDLE ImageHandle, usize *ExitDataSize,
                                 CHAR16 **ExitData);
        EFI_STATUS (*Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
                           usize ExitDataSize, CHAR16 *ExitData);
        EFI_STATUS (*UnloadImage)(EFI_HANDLE ImageHandle);
        /* 29-30 */
        EFI_STATUS (*ConnectController)(EFI_HANDLE ControllerHandle,
                                        EFI_HANDLE *RemainingDevicePath,
                                        BOOLEAN Recursive);
        EFI_STATUS (*DisconnectController)(EFI_HANDLE ControllerHandle,
                                           EFI_HANDLE ChildHandle,
                                           BOOLEAN Recursive);
        /* 31-34 open/close protocol family (opaque slots) */
        void *OpenCloseProtocol[4];
        /* 35 */
        EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, u64 MapKey);
        /* 36 */
        u64 (*GetNextMonotonicCount)(void);
        /* 37 */
        EFI_STATUS (*Stall)(u64 Microseconds);
        /* 38 */
        EFI_STATUS (*SetWatchdogTimer)(u64 TimeoutPeriod, u64 WatchCode,
                                       u64 DataSize, void *Data);
} EFI_BOOT_SERVICES_FULL;

typedef struct EFI_RUNTIME_SERVICES {
        EFI_TABLE_HEADER Hdr;
        void *Svc[32]; /* we never call runtime services pre-handover */
} EFI_RUNTIME_SERVICES;

typedef struct {
        EFI_TABLE_HEADER Hdr;
        CHAR16          *FirmwareVendor;
        u32              FirmwareRevision;
        EFI_HANDLE       ConsoleInHandle;
        EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
        EFI_HANDLE       ConsoleOutHandle;
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
        EFI_HANDLE       StandardErrorHandle;
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
        EFI_RUNTIME_SERVICES *RuntimeServices;
        EFI_BOOT_SERVICES_FULL *BootServices;
        void *ConfigurationTable;
        u64 NumberOfTableEntries;
} EFI_SYSTEM_TABLE;

/* AllocateTypes for AllocatePages */
#define ALL_PAGES 0
#define PALLOC_ANY 1
#define PALLOC_ADDRESS 2

/* LocateHandle search types */
#define BY_ALL_HANDLES 0
#define BY_REGISTERED_PROTOCOL 1
#define BY_PROTOCOL 2

#endif /* MKSTOOR_EFI_H */
