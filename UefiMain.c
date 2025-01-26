#include <Uefi.h>
#include <Library/UefiLib.h>

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE imgHandle, IN EFI_SYSTEM_TABLE* sysTable)
{
  Print(L"Hello, world!\r\n");
  return EFI_SUCCESS;
}
