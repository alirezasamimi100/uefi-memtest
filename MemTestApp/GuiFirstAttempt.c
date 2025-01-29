#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Pi/PiDxeCis.h>
#include <Protocol/MpService.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/PrintLib.h>

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

/* GUI */
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *SimpleTextOut;
UINTN Width, Height;
UINTN DefaultColor = EFI_TEXT_ATTR(EFI_WHITE, EFI_BLUE);
UINTN MarginColor = EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY);
UINTN ProgressColor = EFI_TEXT_ATTR(EFI_WHITE, EFI_GREEN);
UINTN ProgressX1, ProgressX2;
UINTN Padding = 3;
UINTN NTestTotal, NTestDone, NTestPassed;
UINTN PrevReport;
CHAR16 *TestName;
UINTN MessageY;
UINTN PageSteps = 1024;
CHAR16 GuiBuffer[128];

VOID EFIAPI GuiGoto(UINTN X, UINTN Y, UINTN Color) {
  if (Color == 0)
    Color = DefaultColor;
  SimpleTextOut->SetAttribute(SimpleTextOut, Color);
  SimpleTextOut->SetCursorPosition(SimpleTextOut, X, Y);
}

UINTN EFIAPI PrintStringAt(UINTN X, UINTN Y, CHAR16 *String, UINTN Color) {
  GuiGoto(X, Y, Color);
  SimpleTextOut->OutputString(SimpleTextOut, String);
  return StrLen(String);
}

UINTN EFIAPI PrintAt(UINTN X, UINTN Y, CHAR16 Chr, UINTN Color) {
  PrintStringAt(X, Y, (CHAR16[]) {Chr, 0}, Color);
  return 1;
}

UINTN EFIAPI PrintNumberAt(UINTN X, UINTN Y, UINTN Number, UINTN Color) {
  UINTN Top = 0;
  UINTN Len = 0;
  if (Number == 0)
    return PrintAt(X, Y, L'0', Color);
  do {
    GuiBuffer[Top++] = L'0' + (Number % 10);
    Number /= 10;
  } while (Number > 0);
  while (Top > 0) {
    Len += PrintAt(X + Len, Y, GuiBuffer[--Top], Color);    
  }
  return Len;
}

VOID EFIAPI InitProgress(UINTN Y) {
  UINTN X = ProgressX1;
  UINTN Len = ProgressX2 - ProgressX1 + 1;
  PrintAt(X, Y, BOXDRAW_VERTICAL, 0);
  PrintAt(X + Len - 1, Y, BOXDRAW_VERTICAL, 0);
  for (UINTN i = 1; i + 1 < Len; i++) {
    PrintAt(X + i, Y, L' ', 0);
  }
  PrintStringAt(ProgressX2 + 2, Y, L"0%  ", 0);
}

VOID EFIAPI UpdateProgress(UINTN Y, UINTN NOld, UINTN NNew, UINTN NTotal) {
  UINTN X = ProgressX1;
  UINTN Len = ProgressX2 - ProgressX1 + 1;
  X++;
  Len -= 2;
  UINTN KOld = Len * NOld / NTotal;
  UINTN KNew = Len * NNew / NTotal;
  if (KNew <= KOld)
    return;
  for (UINTN i = KOld; i < KNew; i++) {
    PrintAt(X + i, Y, L'#', ProgressColor);
  }
  UINTN CurX = ProgressX2 + 2;
  CurX += PrintNumberAt(CurX, Y, NNew * 100 / NTotal, 0);
  PrintAt(CurX, Y, L'%', 0);
}

EFI_STATUS EFIAPI GuiInit(void) {
  EFI_STATUS Status;
  Status = gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiSimpleTextOutProtocolGuid, (VOID **)&SimpleTextOut);
  if (EFI_ERROR(Status)) {
    SimpleTextOut = NULL;
    Print(L"Failed to handle SimpleTextOut protocol: %r\n", Status);
    return Status;
  }
  Status = SimpleTextOut->QueryMode(SimpleTextOut, SimpleTextOut->Mode->Mode, &Width, &Height);
  
  SimpleTextOut->SetAttribute(SimpleTextOut, DefaultColor);
  SimpleTextOut->ClearScreen(SimpleTextOut);
  SimpleTextOut->EnableCursor(SimpleTextOut, FALSE);

  /* Avoid shifting the terminal */
  Height--;

  for (UINTN i = 0; i < Width; i++) {
    PrintAt(i, 0, L' ', MarginColor);
    PrintAt(i, Height - 1, L' ', MarginColor);
  }
  for (UINTN j = 0; j < Height; j++) {
    PrintAt(0, j, L' ', MarginColor);
    PrintAt(Width - 1, j, L' ', MarginColor);
  }

  CHAR16 *String = L"UEFI Memtest";
  PrintStringAt((Width - StrLen(String)) / 2, 0, String, MarginColor);

  ProgressX1 = 35;
  ProgressX2 = Width - Padding - 1 - 5;

  InitProgress(2);

  MessageY = 16;

  return EFI_SUCCESS;
}

VOID EFIAPI GuiClearLine(UINTN Y, UINTN MaxX) {
  for (UINTN i = 1; i < MaxX; i++) {
    PrintAt(i, Y, L' ', 0);
  }
}

VOID EFIAPI GuiStartTest(CHAR16 *Name) {
  TestName = Name;
  
  UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Test (%lu/%lu):", NTestDone + 1, NTestTotal);
  PrintStringAt(Padding, 2, GuiBuffer, 0);

  // GuiClearLine(4, ProgressX1 - 1);
  UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Progress:", NPage);
  PrintStringAt(Padding, 4, GuiBuffer, 0);
  InitProgress(4);
  PrevReport = 0;

  GuiClearLine(6, Width - 1);
  UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Test name: %s", TestName);
  PrintStringAt(Padding, 6, GuiBuffer, 0);
  GuiGoto(Padding, 6, 0);
}

VOID EFIAPI GuiReport(UINTN CurrentReport, UINTN TotalReport) {
  // UINTN CurX = Padding;
  // CurX += PrintStringAt(CurX, 4, L"Progress (", 0);
  // CurX += PrintNumberAt(CurX, 4, CurrentReport, 0);
  // CurX += PrintAt(CurX, 4, L'/', 0);
  // CurX += PrintNumberAt(CurX, 4, TotalReport, 0);
  // CurX += PrintStringAt(CurX, 4, L"):", 0);

  // UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Page (%lu/%lu):", CurrentPage, NPage);
  // PrintStringAt(Padding, 4, GuiBuffer, 0);
  UpdateProgress(4, PrevReport, CurrentReport, TotalReport);

  PrevReport = CurrentReport;
}

VOID EFIAPI GuiEndTest(BOOLEAN TestResult) {
  UpdateProgress(2, NTestDone, NTestDone + 1, NTestTotal);
  NTestDone++;
  if (TestResult) {
    NTestPassed++;
    UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Test %s passed", TestName);
    PrintStringAt(Padding, MessageY++, GuiBuffer, 0);
  } else {
    UnicodeSPrint(GuiBuffer, sizeof(GuiBuffer), L"Test %s failed", TestName);
    PrintStringAt(Padding, MessageY++, GuiBuffer, 0);
  }
}

VOID EFIAPI Simulate(void) {
  NTestTotal = 3;
  CHAR16 *Name[3] = {L"Walking Ones", L"Identity", L"Rowhammer"};
  for (UINTN i = 0; i < NTestTotal; i++) {
    GuiStartTest(Name[i]);
    for (UINTN j = 0; j < NPage; j += 2000) {
      gBS->Stall(10000);
      GuiReport(j, NPage);
    }
    GuiEndTest(TRUE);
  }
}

VOID EFIAPI GuiWaitExit(void) {
  MessageY++;
  GuiGoto(Padding, MessageY++, 0);
  Print(L"Passed: %lu, Failed: %lu", NTestPassed, NTestTotal - NTestPassed);
  GuiGoto(Padding, MessageY++, 0);
  Print(L"Press any key to exit...");
  gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, NULL);
}

VOID EFIAPI GuiReset(void) {
  if (SimpleTextOut == NULL)
    return;
  SimpleTextOut->EnableCursor(SimpleTextOut, TRUE);
  SimpleTextOut->SetAttribute(SimpleTextOut, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK));
  SimpleTextOut->Reset(SimpleTextOut, TRUE);
}

VOID EFIAPI ReadMemoryMap(void) {
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *MapEntry;

  if (MemoryMap != NULL)
    FreePool(MemoryMap);
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
  NPage = 0;
  MaxPage = 0;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      NPage += MapEntry->NumberOfPages;
      MaxPage = MAX(MaxPage, MapEntry->PhysicalStart / PAGE_SIZE + MapEntry->NumberOfPages);
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
}

BOOLEAN EFIAPI WalkingOnesTest() {
  UINT64 *End;
  UINT64 *Ptr;
  UINT64 Pattern = 0x1;
  ReadMemoryMap();
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
  UINTN SeenPages = 0;
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      UINTN NextReport = 8;
      for (; Ptr < End; Ptr++) {
        *Ptr = Pattern;
        Pattern = Pattern << 1 | Pattern >> 63;
        if (((UINTN) Ptr - MapEntry->PhysicalStart) / PAGE_SIZE >= NextReport) {
          GuiReport(SeenPages + NextReport, NPage);
          NextReport += PageSteps;
        }
      }
      SeenPages += MapEntry->NumberOfPages;
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
          GuiGoto(Padding, 4, 0);
          Print(L"Error at %p, expected %lx, got %lx\r\n", Ptr, Pattern, *Ptr);
          return FALSE;
        }
        Pattern = Pattern << 1 | Pattern >> 63;
      }
    }
    MapEntry = NEXT_MEMORY_DESCRIPTOR(MapEntry, DescSize);
  }
  GuiReport(NPage, NPage);
  return TRUE;
}

VOID EFIAPI IdentityTestWorker(VOID* OUT Res) {
  UINTN ID;
  UINT64 *End;
  UINT64 *Ptr;
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
  UINTN SeenPages = 0;
  gMpService->WhoAmI(gMpService, &ID);
  while (MapEntry < MapEnd) {
    if (MapEntry->Type == EfiConventionalMemory) {
      Ptr = (UINT64 *)MapEntry->PhysicalStart + ID;
      End = (UINT64 *)(MapEntry->PhysicalStart + MapEntry->NumberOfPages * PAGE_SIZE);
      UINTN NextReport = PageSteps;
      for (; Ptr < End; Ptr += NProc) {
        *Ptr = (UINT64)Ptr;
        if (ID == 0 && ((UINTN) Ptr - MapEntry->PhysicalStart) / PAGE_SIZE >= NextReport) {
          GuiReport(SeenPages + NextReport, NPage);
          NextReport += PageSteps;
        }
      }
      SeenPages += MapEntry->NumberOfPages;
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
  ReadMemoryMap();
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
  GuiReport(NPage, NPage);
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
  BOOLEAN *PageValid, HasValid;
  PageValid = AllocateZeroPool((MaxPage + 7) / 8);
  ReadMemoryMap();
  EFI_MEMORY_DESCRIPTOR *MapEntry = MemoryMap;
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
  Status = GuiInit();
  if (EFI_ERROR(Status)) {
    Print(L"Failed to initialize GUI: %r\n", Status);
    return Status;
  }

  // Simulate();
  BOOLEAN Result;
  NTestTotal = 2;
  
  GuiStartTest(L"walking ones");
  Result = WalkingOnesTest();
  GuiEndTest(Result);

  GuiStartTest(L"identity");
  Result = IdentityTest();
  GuiEndTest(Result);

  // GuiStartTest(L"rowhammer");
  // Result = RowHammerTest();
  // GuiEndTest(Result);

  // Print(L"Total number of pages %lu\r\n", NPage);
  // if (!WalkingOnesTest()) {
  //   Print(L"Walking ones test failed\r\n");
  // } else {
  //   Print(L"Walking ones test passed\r\n");
  // }
  // if (!IdentityTest()) {
  //   Print(L"Identity test failed\r\n");
  // } else {
  //   Print(L"Identity test passed\r\n");
  // }
  // if (!RowHammerTest()) {
  //   Print(L"Row hammer test failed\r\n");
  // } else {
  //   Print(L"Row hammer test passed\r\n");
  // }
  FreePool(MemoryMap);
  GuiWaitExit();
  GuiReset();
  return EFI_SUCCESS;
}
