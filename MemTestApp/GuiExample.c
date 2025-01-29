#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Library/DebugLib.h>

EFI_HII_HANDLE HiiHandle;
EFI_STATUS Status;
EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput;
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *SimpleTextOut;
UINT64 NRow, NCol, Width, Height;

VOID PrintAt(UINTN X, UINTN Y, CHAR16 Chr) {
    SimpleTextOut->SetCursorPosition(SimpleTextOut, X, Y);
    SimpleTextOut->OutputString(SimpleTextOut, (CHAR16[]) {Chr, 0});
}

VOID PrintStringAt(UINTN X, UINTN Y, CHAR16 *String) {
    SimpleTextOut->SetCursorPosition(SimpleTextOut, X, Y);
    SimpleTextOut->OutputString(SimpleTextOut, String);
}

VOID DrawBox(UINTN X, UINTN Y, UINTN W, UINTN H) {
    PrintAt(X, Y, BOXDRAW_DOWN_RIGHT);
    PrintAt(X + W - 1, Y, BOXDRAW_DOWN_LEFT);
    PrintAt(X, Y + H - 1, BOXDRAW_UP_RIGHT);
    PrintAt(X + W - 1, Y + H - 1, BOXDRAW_UP_LEFT);
    for (UINTN i = 1; i + 1 < W; i++) {
        PrintAt(X + i, Y, BOXDRAW_HORIZONTAL);
        PrintAt(X + i, Y + H - 1, BOXDRAW_HORIZONTAL);
    }
    for (UINTN j = 1; j + 1 < H; j++) {
        PrintAt(X, Y + j, BOXDRAW_VERTICAL);
        PrintAt(X + W - 1, Y + j, BOXDRAW_VERTICAL);
    }
}

EFI_STATUS EFIAPI UefiEntry(IN EFI_HANDLE imgHandle, IN EFI_SYSTEM_TABLE* sysTable)
{
    gST = sysTable;
    gBS = sysTable->BootServices;
    gImageHandle = imgHandle;

    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&GraphicsOutput);
    if (EFI_ERROR(Status)) {
        Print(L"graphics failed: %r\n", Status);
        return Status;
    }
    Status = gBS->HandleProtocol(gST->ConsoleOutHandle, &gEfiSimpleTextOutProtocolGuid, (VOID **)&SimpleTextOut);
    if (EFI_ERROR(Status)) {
        Print(L"text failed: %r\n", Status);
        return Status;
    }
    
    Width = GraphicsOutput->Mode->Info->HorizontalResolution;
    Height = GraphicsOutput->Mode->Info->VerticalResolution;
    Print(L"Graphics size width = %lu, height = %lu\n", Width, Height);
    Status = SimpleTextOut->QueryMode(SimpleTextOut, SimpleTextOut->Mode->Mode, &NCol, &NRow);
    Print(L"Text size row = %lu, col = %lu\n", NRow, NCol);
    
    SimpleTextOut->SetAttribute(SimpleTextOut, EFI_TEXT_ATTR(EFI_WHITE, EFI_MAGENTA));
    SimpleTextOut->ClearScreen(SimpleTextOut);
    SimpleTextOut->EnableCursor(SimpleTextOut, FALSE);
    
    CHAR16 String[] = L"Solam Donya  ";
    UINTN NString = StrLen(String);
    UINTN BoxW = NString + 4, BoxH = 3;
    UINTN BoxX = (NCol - BoxW) / 2, BoxY = (NRow - BoxH) / 2;
    DrawBox(BoxX, BoxY, BoxW, BoxH);
    PrintStringAt(BoxX + 2, BoxY + 1, String);
    
    CHAR16 Loading[] = L"|/-\\";
    for (UINTN i = 0; i < 1000; i++) {
        PrintAt(BoxX + 2 + NString - 1, BoxY + 1, Loading[i % 4]);
        gBS->Stall(200000);
    }

    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, NULL);
    SimpleTextOut->EnableCursor(SimpleTextOut, TRUE);
    SimpleTextOut->Reset(SimpleTextOut, TRUE);

    return EFI_SUCCESS;
}
