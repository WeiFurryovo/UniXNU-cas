// XNU adapter extraction; source hashes in transport-sources.json.
/** @file
  umi SPMI v5 observer reads, EE0. Source contract: LineageOS SM8250
  71b13e62f057a649b77fe4062feb73ee72ad609c, drivers/spmi/spmi-pmic-arb.c.
  Same mapping and read format verified by UmiButtonsDxe on this handset.
  Ownership/configuration are never changed. PMIC writes are restricted to
  primary PON reset registers, plus a dedicated guarded umi board-boost enable.
  Touch LDO power uses RPMh; its registers remain read-only through this library.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
STATIC UINTN mBoardWriteApid = 512;
STATIC UINT32 mBoardWriteOwner = 7;
STATIC UINTN mBoardWriteChannel;
STATIC EFI_STATUS
Transfer (UINT8 Sid, UINT16 Address, UINT8 *Value, BOOLEAN Write)
{
  UINTN Index;
  UINTN Apid = 512;
  UINTN Channel;
  UINT32 Mapping;
  UINT32 Owner;
  UINT32 SelectedOwner = 7;
  UINT32 Status;
  UINT16 Ppid;
  BOOLEAN Interrupts;
  EFI_STATUS Result = EFI_TIMEOUT;

  if ((Sid > 15) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MmioRead32 (0x0C440000) != 0x50010000) {
    return EFI_UNSUPPORTED;
  }
  Ppid = ((UINT16)Sid << 8) | (Address >> 8);
  for (Index = 0; Index < 512; Index++) {
    Mapping = MmioRead32 (0x0C440900 + Index * 4);
    if ((Mapping == 0) || (((Mapping >> 8) & 0xFFF) != Ppid)) {
      continue;
    }
    Owner = MmioRead32 (0x0C40A700 + Index * 4) & 7;
    if ((Apid == 512) || (Owner == 0)) {
      Apid = Index;
      SelectedOwner = Owner;
    }
  }
  if (Apid == 512) {
    return EFI_NOT_FOUND;
  }
  if (Write) {
    mBoardWriteApid = Apid;
    mBoardWriteOwner = SelectedOwner;
  }
  if (Write && (SelectedOwner != 0)) {
    return EFI_ACCESS_DENIED;
  }
  Channel = Write ? 0x0C600000 + Apid * 0x10000 : 0x0E600000 + Apid * 0x80;
#ifdef UMI_SPMI_PREPARE_WRITE
  if (Write && !UMI_SPMI_PREPARE_WRITE(Sid, Address, *Value, Channel)) {
    return EFI_ACCESS_DENIED;
  }
#endif
  if (Write) mBoardWriteChannel = Channel;
  Interrupts = SaveAndDisableInterrupts ();
  if (Write) {
    MmioWrite32 (Channel + 0x10, *Value);
  }
  MmioWrite32 (Channel, (Write ? 0 : (1U << 27)) | ((Address & 0xFFU) << 4));
  for (Index = 0; Index < 1000; Index++) {
    Status = MmioRead32 (Channel + 0x08);
    if ((Status & BIT0) != 0) {
      if ((Status & (BIT1 | BIT2 | BIT3)) != 0) {
        Result = EFI_DEVICE_ERROR;
        break;
      }
      if (!Write) {
        *Value = (UINT8)MmioRead32 (Channel + 0x18);
      }
      Result = EFI_SUCCESS;
      break;
    }
    MicroSecondDelay (1);
  }
  SetInterruptState (Interrupts);
  return Result;
}

EFI_STATUS
UmiSpmiRead8 (UINT8 Sid, UINT16 Address, UINT8 *Value)
{
  return Transfer (Sid, Address, Value, FALSE);
}

EFI_STATUS
UmiSpmiWrite8 (UINT8 Sid, UINT16 Address, UINT8 Value)
{
  BOOLEAN Pon = (Sid == 0) &&
    ((Address == 0x085A) || (Address == 0x085B) || (Address == 0x088F));
  if (!Pon) {
    return EFI_ACCESS_DENIED;
  }
  return Transfer (Sid, Address, &Value, TRUE);
}

// Caller verifies PM8150B identity and exact enabled GPIO-output profile first.
// Fixed operation prevents other PMIC, GPIO, voltage or NVM writes.
STATIC EFI_STATUS
UmiSpmiBoardBoostHigh (VOID)
{
  UINT8 Value = 0x80;
  return Transfer (2, 0xC444, &Value, TRUE);
}
