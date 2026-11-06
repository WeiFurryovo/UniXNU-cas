// SPDX-License-Identifier: BSD-2-Clause-Patent
// UM10204 3.1.16 bus clear, restricted to our interrupted 0x86 FIFO read.
// No START, data write, reset or supply change is issued by recovery.
STATIC UINT32 mRecoveryPulses, mRecoveryLines, mRecoveryResult;
STATIC UINT64 mRecoveryElapsedNs;
STATIC BOOLEAN
RecoveryEligible (UINT32 Result)
{
  return !mNative && Result == EFI_TIMEOUT && mLastFifoRead && mLastPhase == 7 &&
         mAckMask == 7 && mLastStopResult == EFI_SUCCESS && mLastLines == 2 &&
         mOwned && mLastPinsValid && PowerReady () &&
         MmioRead32 (GPIO_CTL (SDA)) == mGpioCtl[0] &&
         MmioRead32 (GPIO_CTL (SCL)) == mGpioCtl[1] &&
         !(MmioRead32 (SE13_BASE + 0x40) & GENI_BUSY_MASK) &&
         !High (SDA) && High (SCL);
}

STATIC EFI_STATUS
RecoverRead (UINT32 Result)
{
  mRecoveryPulses = 0; mRecoveryElapsedNs = 0;
  mRecoveryLines = mLastLines; mRecoveryResult = EFI_UNSUPPORTED;
  if (!RecoveryEligible (Result)) return EFI_UNSUPPORTED;
  mTransferStart = GetPerformanceCounter ();
  mTransferLimitNs = CLEANUP_LIMIT_NS;
  EFI_STATUS Status = EFI_SUCCESS;
  // SDA stays released: completing the interrupted byte produces a NACK.
  // Nine clocks cover any remaining data bits plus the NACK clock. A high
  // data bit alone is not proof that the target has left transmit mode.
  for (UINT32 i = 0; i < 9; ++i) {
    ClockLow ();
    Status = ClockHigh ();
    if (EFI_ERROR (Status)) break;
    ++mRecoveryPulses;
  }
  if (!EFI_ERROR (Status)) {
    if (!High (SDA)) Status = EFI_NOT_READY;
    else Status = Stop (); // Same cleanup deadline; no fresh budget.
  }
  Line (SDA, TRUE); Line (SCL, TRUE);
  mRecoveryLines = (High (SDA) ? 1U : 0U) | (High (SCL) ? 2U : 0U);
  if (!EFI_ERROR (Status) && mRecoveryLines != 3) Status = EFI_NOT_READY;
  if (!EFI_ERROR (Status) && (!PowerReady () ||
      MmioRead32 (GPIO_CTL (SDA)) != mGpioCtl[0] ||
      MmioRead32 (GPIO_CTL (SCL)) != mGpioCtl[1] ||
      (MmioRead32 (SE13_BASE + 0x40) & GENI_BUSY_MASK))) Status = EFI_ACCESS_DENIED;
  mRecoveryElapsedNs = GetTimeInNanoSecond (GetPerformanceCounter () - mTransferStart);
  mRecoveryResult = Status;
  return Status;
}
