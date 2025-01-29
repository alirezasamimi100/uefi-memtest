#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Pi/PiDxeCis.h>
#include <Protocol/MpService.h>

enum {
  PAGE_SIZE     = 1 << 12,
  ROW_SIZE      = 1 << 13,
  PAGES_IN_ROW  = ROW_SIZE / PAGE_SIZE,
  NUM_READS     = 1024,
};

EFI_MP_SERVICES_PROTOCOL *gMpService;
EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
EFI_MEMORY_DESCRIPTOR *MapEnd;
UINTN NProc;
UINTN NProcEnabled;
UINTN DescSize;
UINTN MapKey;
UINT32 DescVer;
UINT64 NPage = 0;
UINTN MemMapSize = 0;
UINT64 MaxPage = 0;

BOOLEAN EFIAPI WalkingOnesTest() {
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

VOID EFIAPI IdentityTestWorker(VOID* OUT Res) {
  UINTN ID;
  UINT64 *End;
  UINT64 *Ptr;
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
  gMpService->WhoAmI(gMpService, &ID);
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart + ID;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      for (; Ptr < End; Ptr += NProc) {
        *Ptr = (UINT64)Ptr;
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  WriteBackInvalidateDataCache();
  MapEntry = MemoryMap;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart + ID;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      for (; Ptr < End; Ptr += NProc) {
        if (*Ptr != (UINT64)Ptr) {
          *(UINT64 **)Res = Ptr;
          return;
        }
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
}

BOOLEAN EFIAPI IdentityTest() {
  EFI_STATUS Status;
  EFI_EVENT Event;
  UINT64* Res = (UINT64*) ~0x0ULL;
  Status = gBS->CreateEvent(0, TPL_NOTIFY, NULL, NULL, &Event);
  if (EFI_ERROR(Status)) {
    Print(L"Failed to create event: %r\r\n", Status);
    return FALSE;
  }
  Status = gMpService->StartupAllAPs(gMpService, IdentityTestWorker, FALSE, Event, 0, &Res, NULL);
  if (EFI_ERROR(Status)) {
    Print(L"Failed to start APs: %r\r\n", Status);
    return FALSE;
  }
  IdentityTestWorker(&Res);
  Status = gBS->WaitForEvent(1, &Event, NULL);
  if (EFI_ERROR(Status)) {
    Print(L"Failed to wait for event: %r\r\n", Status);
    return FALSE;
  }
  if (Res != (UINT64*) ~0x0ULL) {
    Print(L"Error at %p, expected %lx, got %lx\r\n", Res, (UINT64)Res, *Res);
    return FALSE;
  }
  return TRUE;
}

UINT64 EFIAPI Hammer(UINT64 *FirstPtr, UINT64 *SecondPtr) {
  UINT64 i;
  UINT64 sum = 0;
  for (i = 0; i < NUM_READS; i++) {
    sum += FirstPtr[0];
    sum += SecondPtr[0];
    WriteBackInvalidateDataCacheRange(FirstPtr, 8);
    WriteBackInvalidateDataCacheRange(SecondPtr, 8);
  }
  return sum;
}

BOOLEAN EFIAPI RowHammerTest() {
  UINT64 Page;
  UINT64 DPage, UPage, MPage;
  UINT64 *Ptr;
  UINT64 i, j, k;
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
  BOOLEAN *PageValid, HasValid;
  PageValid = AllocateZeroPool((MaxPage + 7) / 8);
  MapEntry = MemoryMap;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Page = MapEntry->PhysicalStart / PAGE_SIZE;
      for (i = 0; i < MapEntry->NumberOfPages; i++, Page++) {
        PageValid[Page / 8] |= 1 << (Page % 8);
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  for (Page = PAGES_IN_ROW; Page + PAGES_IN_ROW < MaxPage; Page += PAGES_IN_ROW) {
    for (i = 0; i < PAGES_IN_ROW; i++) {
      UPage = Page - PAGES_IN_ROW + i;
      if (!(PageValid[UPage / 8] & (1 << (UPage % 8)))) {
        continue;
      }
      for (j = 0; j < PAGES_IN_ROW; j++) {
        DPage = Page + PAGES_IN_ROW + j;
        if (!(PageValid[DPage / 8] & (1 << (DPage % 8)))) {
          continue;
        }
        HasValid = FALSE;
        for (k = 0; k < PAGES_IN_ROW; k++) {
          MPage = Page + k;
          if (!(PageValid[MPage / 8] & (1 << (MPage % 8)))) {
            continue;
          }
          HasValid = TRUE;
          Ptr = (UINT64 *)(MPage * PAGE_SIZE);
          SetMem(Ptr, PAGE_SIZE, 0xff);
          WriteBackInvalidateDataCacheRange(Ptr, PAGE_SIZE);
        }
        if (HasValid) {
            Hammer((UINT64 *)(UPage * PAGE_SIZE), (UINT64 *)(DPage * PAGE_SIZE));
            for (k = 0; k < PAGES_IN_ROW; k++) {
              MPage = Page + k;
              if (!(PageValid[MPage / 8] & (1 << (MPage % 8)))) {
                continue;
              }
              Ptr = (UINT64 *)(MPage * PAGE_SIZE);
              if (*Ptr != 0xffffffffffffffff) {
                Print(L"Row hammer detected at %p\r\n", Ptr);
                return FALSE;
              }
              if (CompareMem(Ptr, Ptr + 1, PAGE_SIZE - 8) != 0) {
                Print(L"Row hammer detected at %p\r\n", Ptr);
                return FALSE;
              }
          }
        }
      }
    }
  }
  FreePool(PageValid);
  return TRUE;
}

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE imgHandle,
                            IN EFI_SYSTEM_TABLE *sysTable) {
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *MapEntry;
  // UEFI apps automatically exit after 5 minutes. Stop that here
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  Status = gBS->LocateProtocol(&gEfiMpServiceProtocolGuid, NULL,
                               (VOID **)&gMpService);
  if (EFI_ERROR(Status)) {
    Print(L"Failed to locate MpService Protocol: %r\r\n", Status);
    return Status;
  }
  Status = gMpService->GetNumberOfProcessors(gMpService, &NProc, &NProcEnabled);
  if (EFI_ERROR(Status)) {
    Print(L"Failed to get number of processors: %r\r\n", Status);
    return Status;
  }

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
      MaxPage = MAX(MaxPage, MapEntry->PhysicalStart / PAGE_SIZE + MapEntry->NumberOfPages);
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  Print(L"Total number of pages %lu\r\n", NPage);
  if (!WalkingOnesTest()) {
    Print(L"Walking ones test failed\r\n");
  } else {
    Print(L"Walking ones test passed\r\n");
  }
  if (!IdentityTest()) {
    Print(L"Identity test failed\r\n");
  } else {
    Print(L"Identity test passed\r\n");
  }
  if (!RowHammerTest()) {
    Print(L"Row hammer test failed\r\n");
  } else {
    Print(L"Row hammer test passed\r\n");
  }
  FreePool(MemoryMap);
  return EFI_SUCCESS;
}
