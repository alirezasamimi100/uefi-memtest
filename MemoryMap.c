#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/CacheMaintenanceLib.h>

enum {
    PAGE_SIZE = 1 << 12,
};

BOOLEAN TestRegion(IN EFI_PHYSICAL_ADDRESS start, UINT64 len) {
    Print(L"Test from %lx with length %lu\n", start, len);

    volatile UINT64 *Mem = (UINT64 *) start;
    const UINT64 NWord = len / sizeof(*Mem);
    UINT64 i;
    for (i = 0; i < NWord; i++) {
        Mem[i] = 0;
    }
    WriteBackInvalidateDataCacheRange((VOID *)&Mem[0], len);
    for (i = 0; i < NWord; i++) {
        if (Mem[i] != 0) {
            Print(L"test failed\n");
            return FALSE;
        }
    }
    return TRUE;
}

EFI_STATUS EFIAPI UefiEntry(IN EFI_HANDLE imgHandle, IN EFI_SYSTEM_TABLE* sysTable) {
    EFI_STATUS Status;
    UINTN MemMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL, *MapEntry, *MapEnd;
    UINTN MapKey, DescSize;
    UINT32 DescVer;

    gST = sysTable;
    gBS = sysTable->BootServices;
    gImageHandle = imgHandle;
    // UEFI apps automatically exit after 5 minutes. Stop that here
    gBS->SetWatchdogTimer(0, 0, 0, NULL);

    Status = gBS->GetMemoryMap(&MemMapSize, MemoryMap, &MapKey, &DescSize, &DescVer);
    do {
        MemoryMap = AllocatePool(MemMapSize);
        if (MemoryMap == NULL)
            ASSERT(FALSE);
        Status = gBS->GetMemoryMap(&MemMapSize, MemoryMap, &MapKey, &DescSize, &DescVer);
        if (EFI_ERROR(Status))
            FreePool(MemoryMap);
    } while (Status == EFI_BUFFER_TOO_SMALL);
    MapEntry = MemoryMap;
    MapEnd = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + MemMapSize);
    UINT64 NPage = 0;
    while (MapEntry < MapEnd) {
        if (MapEntry->Type == EfiConventionalMemory) {
            TestRegion(MapEntry->PhysicalStart, MapEntry->NumberOfPages * PAGE_SIZE);
            NPage += MapEntry->NumberOfPages;
        }
        MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
    }
    Print(L"Total number of pages %lu\n", NPage);
    FreePool(MemoryMap);
    return EFI_SUCCESS;
}
