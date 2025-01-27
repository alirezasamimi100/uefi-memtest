#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>


enum {
  PAGE_SIZE = 1 << 12,
};

EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
EFI_MEMORY_DESCRIPTOR *MapEnd;
UINTN DescSize;
UINTN MapKey;
UINT32 DescVer;
UINT64 NPage = 0;
UINTN MemMapSize = 0;

BOOLEAN WalkingOnesTest() {
  UINT64 *End;
  UINT64 *Ptr;
  UINT64 Pattern = 0x1;
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      for (; Ptr < End; Ptr++) {
        *Ptr = Pattern;
        Pattern = Pattern << 1 | Pattern >> 63;
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  WriteBackInvalidateDataCache();
  Pattern = 0x1;
  MapEntry = MemoryMap;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      for (; Ptr < End; Ptr++) {
        if (*Ptr != Pattern) {
          Print(L"Error at %p, expected %lx, got %lx\r\n", Ptr, Pattern, *Ptr);
          return FALSE;
        }
        Pattern = Pattern << 1 | Pattern >> 63;
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  return TRUE;
}

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE imgHandle,
                            IN EFI_SYSTEM_TABLE *sysTable) {
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *MapEntry;
  // UEFI apps automatically exit after 5 minutes. Stop that here
  gBS->SetWatchdogTimer(0, 0, 0, NULL);

  Status = gBS->GetMemoryMap(&MemMapSize, MemoryMap, &MapKey, &DescSize, &DescVer);
  do {
    MemoryMap = AllocatePool(MemMapSize);
    if (MemoryMap == NULL)
      ASSERT(FALSE);
    Status =
        gBS->GetMemoryMap(&MemMapSize, MemoryMap, &MapKey, &DescSize, &DescVer);
    if (EFI_ERROR(Status))
      FreePool(MemoryMap);
  } while (Status == EFI_BUFFER_TOO_SMALL);
  MapEntry = MemoryMap;
  MapEnd = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + MemMapSize);
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      NPage += MapEntry->NumberOfPages;
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  Print(L"Total number of pages %lu\r\n", NPage);
  if (!WalkingOnesTest()) {
    Print(L"Walking ones test failed\r\n");
  } else {
    Print(L"Walking ones test passed\r\n");
  }
  FreePool(MemoryMap);
  return EFI_SUCCESS;
}
