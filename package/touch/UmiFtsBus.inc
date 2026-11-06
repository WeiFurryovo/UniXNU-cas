// XNU adapter extraction; source hashes in transport-sources.json.
#include "UmiFtsPowerSequence.h"
#include "UmiFtsPinConfig.h"
#define SE13_BASE          0x00A94000U
#define TLMM_SOUTH         0x0F500000U
#define GPIO_CTL(N)        (TLMM_SOUTH + (N) * 0x1000U)
#define GPIO_IO(N)         (GPIO_CTL (N) + 4U)
#define SDA                36U
#define SCL                37U
#define GPIO_FUNC_MASK     0x3CU
#define GPIO_PULL_MASK     0x3U
#define GPIO_DRIVE_MASK    0x1C0U
#define GPIO_OE            BIT9
#define GPIO_OUT           BIT1
#define GPIO_IN            BIT0
#define GENI_BUSY_MASK     (BIT0 | BIT12)
#define HALF_CYCLE_US      5U
#define STRETCH_LIMIT_US   500U
#define TRANSFER_LIMIT_NS  32000000ULL
#define CLEANUP_LIMIT_NS   8000000ULL
#define POLL_INTERVAL      200000ULL /* 20 ms, in UEFI 100 ns units. */
#define MAX_RECORDS        4U

STATIC BOOLEAN   mOwned;
STATIC BOOLEAN   mNative;
STATIC UINT32    mSavedCtl[2];
STATIC UINT32    mSavedOut[2];
STATIC UINT32    mGpioCtl[2];
STATIC BOOLEAN   mIrqOwned;
STATIC UINT32    mSavedIrqCtl,mActiveIrqCtl;
STATIC UINT64    mTransferStart;
STATIC UINT64    mTransferLimitNs = TRANSFER_LIMIT_NS;
STATIC BOOLEAN   mLastFifoRead;
STATIC UINT32    mAckMask;
STATIC UINT32    mAckIndex;
// Per-transfer diagnostic state. Failed bytes are never consumed as events.
STATIC UINT32 mCurrentPhase,mCurrentByte,mLastPhase,mLastByte,mLastStopResult;
STATIC UINT32 mLastLines,mLastPinsValid,mLastGeni;
STATIC UINT64 mClockLast,mMaxGapNs,mLastElapsedNs,mLastMaxGapNs;
STATIC BOOLEAN mPowerOwned;
STATIC BOOLEAN mAwaitReady;
STATIC UINT8 mSupplySid;
STATIC UINT8 mSupplyEnable;
STATIC UINT32 mPowerCtl[2];
STATIC UINT32 mPowerOut[2];
STATIC UINT64 mReadyStart;
STATIC BOOLEAN mCycleRequested;
STATIC BOOLEAN mCycleEnabled;
STATIC UmiFtsAvddCycle mAvddCycle;
STATIC UINT64 mCycleElapsedNs;
STATIC BOOLEAN mFullCycleEnabled,mFullCycleRequested;
STATIC UmiFtsFullCycle mFullCycle;
STATIC UINT64 mFullCycleElapsedNs;

STATIC UmiFtsSupplyState
ReadSupplyState (VOID)
{
  UmiFtsSupplyState r={};UINT8 value[4]={};
  STATIC CONST UINT16 addresses[]={0x4046,0x4008,0x4042,0x4043};
  for (unsigned n=0;n<4;++n) {
    r.result=UmiSpmiRead8(mSupplySid,addresses[n],value+n);
    if (r.result) break;
    r.valid|=1U<<n;
  }
  r.enable=value[0];r.status=value[1];r.mv=value[2]|(unsigned(value[3])<<8);
  return r;
}

STATIC VOID
Snapshot (VOID)
{
  STATIC CONST UINT32 Pins[] = {36, 37, 38, 39, 69};
  STATIC CONST UINT32 Offsets[] = {0x40, 0x48, 0x64, 0x68, 0x7C, 0x908};
  UINTN Index;
  for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
    DEBUG ((DEBUG_WARN, "UMI touch SE13+%03x=%08x\n", Offsets[Index],
            MmioRead32 (SE13_BASE + Offsets[Index])));
  }
  for (Index = 0; Index < ARRAY_SIZE (Pins); Index++) {
    DEBUG ((DEBUG_WARN, "UMI touch GPIO%u ctl=%08x io=%08x\n", Pins[Index],
            MmioRead32 (GPIO_CTL (Pins[Index])), MmioRead32 (GPIO_IO (Pins[Index]))));
  }
}

STATIC BOOLEAN
High (UINT32 Pin)
{
  return (MmioRead32 (GPIO_IO (Pin)) & GPIO_IN) != 0;
}

STATIC BOOLEAN
PowerReady (VOID)
{
  UINT32 Pin;
  UINT32 Ctl;
  UINT32 Io;
  /* Digital state only; this is not a rail voltage measurement. */
  for (Pin = 38; Pin <= 69; Pin += 31) {
    Ctl = MmioRead32 (GPIO_CTL (Pin));
    Io = MmioRead32 (GPIO_IO (Pin));
    if (((Ctl & GPIO_FUNC_MASK) != 0) || ((Ctl & GPIO_OE) == 0) ||
        ((Io & (GPIO_OUT | GPIO_IN)) != (GPIO_OUT | GPIO_IN))) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC VOID
RestorePower (VOID)
{
  UINTN Index;
  UINT32 Pin;
  EFI_STATUS Status;
  if (mPowerOwned) {
    // Restore reset first, then AVDD. Original pins are recorded before writes.
    for (Index = 0; Index < 2; Index++) {
      Pin = (Index == 0) ? 38 : 69;
      MmioWrite32 (GPIO_CTL (Pin), MmioRead32 (GPIO_CTL (Pin)) & ~GPIO_OE);
      MmioWrite32 (GPIO_IO (Pin), (MmioRead32 (GPIO_IO (Pin)) & ~GPIO_OUT) | mPowerOut[Index]);
      MmioWrite32 (GPIO_CTL (Pin), mPowerCtl[Index]);
    }
    mPowerOwned = FALSE;
  }
  if (UmiRpmhTouchOutstanding ()) {
    Status = UmiRpmhTouchDisable ();
    DEBUG ((DEBUG_WARN, "UMI touch withdraw RPMh enable status=%d outstanding=%u\n",
            Status, UmiRpmhTouchOutstanding ()));
    (VOID)Status;
  }
}

STATIC VOID
DrivePowerPin (UINT32 Pin, BOOLEAN HighValue)
{
  UINT32 Ctl = umi_fts_power_pin_config(MmioRead32(GPIO_CTL(Pin)),Pin);
  MmioWrite32 (GPIO_CTL (Pin), Ctl);
  MmioWrite32 (GPIO_IO (Pin),
               (MmioRead32 (GPIO_IO (Pin)) & ~GPIO_OUT) | (HighValue ? GPIO_OUT : 0));
  MmioWrite32 (GPIO_CTL (Pin), Ctl | GPIO_OE);
  MemoryFence ();
}

STATIC EFI_STATUS
InitializePower (VOID)
{
  STATIC CONST UINT8 Sids[] = {0, 2, 4, 8, 10};
  STATIC CONST UINT16 Registers[] = {0x4004, 0x4005, 0x4008, 0x4040, 0x4041,
                                    0x4042, 0x4043, 0x4045, 0x4046};
  UINT8 Type;
  UINT8 Model;
  UINT8 Values[ARRAY_SIZE (Registers)];
  UINT8 Ready = 0;
  UINT8 Enable = 0;
  UINTN Index;
  UINTN Matches = 0;
  EFI_STATUS Status;

  // RPMh "ldoc1" names PM8150A L1. Find the unique physical PM8150A rather
  // than trusting a historical DT node name. The original PmicDxe reads
  // type/model at 104/105 and enumerates primary/secondary SID pairs.
  for (Index = 0; Index < ARRAY_SIZE (Sids); Index++) {
    Type = Model = 0;
    Status = UmiSpmiRead8 (Sids[Index], 0x0104, &Type);
    if (!EFI_ERROR (Status)) {
      Status = UmiSpmiRead8 (Sids[Index], 0x0105, &Model);
    }
    DEBUG ((DEBUG_WARN, "UMI touch PMIC SID%u type=%02x model=%02x status=%d\n",
            Sids[Index], Type, Model, Status));
    if (!EFI_ERROR (Status) && (Type == 0x51) && (Model == 0x27)) {
      Matches++;
      mSupplySid = Sids[Index] + 1;
    }
  }
  if (Matches != 1) {
    return EFI_UNSUPPORTED;
  }
  for (Index = 0; Index < ARRAY_SIZE (Registers); Index++) {
    Values[Index] = 0;
    Status = UmiSpmiRead8 (mSupplySid, Registers[Index], &Values[Index]);
    DEBUG ((DEBUG_WARN, "UMI touch L1 SID%u reg=%04x value=%02x status=%d\n",
            mSupplySid, Registers[Index], Values[Index], Status));
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  // This handset read type04/subtype71 on attempt12. Reuse only that observed
  // peripheral and an existing 1800mV setting; no voltage/mode programming.
  if ((Values[0] != 4) || (Values[1] != 0x71) ||
      (((UINT16)Values[4] << 8) | Values[3]) != 1800) {
    return EFI_UNSUPPORTED;
  }
  mSupplyEnable = Values[8];
  if ((mSupplyEnable & BIT7) == 0) {
    // The observed aggregate software enable must be exactly zero. Leave
    // pin-controlled or otherwise unfamiliar enable values untouched.
    if ((mSupplyEnable != 0) || ((Values[2] & BIT7) != 0)) {
      return EFI_UNSUPPORTED;
    }
    // This requests ldoc1 enable through APPS DRV2. Cleanup sends data0 for
    // our request; it cannot reconstruct an unknown inherited DRV vote.
    Status = UmiRpmhTouchEnable ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = UmiSpmiRead8 (mSupplySid, 0x4046, &Enable);
  DEBUG ((DEBUG_WARN, "UMI touch L1 after vote enable=%02x status=%d\n", Enable, Status));
  if (EFI_ERROR (Status) || ((Enable & BIT7) == 0)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  for (Index = 0; Index < 100; Index++) {
    Status = UmiSpmiRead8 (mSupplySid, 0x4008, &Ready);
    if (EFI_ERROR (Status) || ((Ready & BIT7) != 0)) {
      break;
    }
    MicroSecondDelay (1000);
  }
  if (Index == 100) {
    Status = EFI_TIMEOUT;
  }
  DEBUG ((DEBUG_WARN, "UMI touch L1 ready=%02x polls=%u status=%d\n", Ready,
          (UINT32)((Index < 100) ? Index + 1 : 100), Status));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Values[5] = Values[6] = 0;
  Status = UmiSpmiRead8 (mSupplySid, 0x4042, &Values[5]);
  if (!EFI_ERROR (Status)) {
    Status = UmiSpmiRead8 (mSupplySid, 0x4043, &Values[6]);
  }
  DEBUG ((DEBUG_WARN, "UMI touch L1 voltage readback=%02x/%02x mV=%u status=%d\n",
          Values[5], Values[6], (UINT32)(((UINT16)Values[6] << 8) | Values[5]), Status));
  if (EFI_ERROR (Status) || ((((UINT16)Values[6] << 8) | Values[5]) != 1800)) {
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }
  MicroSecondDelay (15000); // fts_enable_reg: VDD, 15 ms, then AVDD.
  for (Index = 0; Index < 2; Index++) {
    UINT32 Pin = (Index == 0) ? 38 : 69;
    mPowerCtl[Index] = MmioRead32 (GPIO_CTL (Pin));
    mPowerOut[Index] = MmioRead32 (GPIO_IO (Pin)) & GPIO_OUT;
  }
  mPowerOwned = TRUE;
  if (mFullCycleEnabled) {
    mFullCycleRequested=TRUE;UINT64 started=GetPerformanceCounter();
    mFullCycle=umi_fts_full_cycle(mSupplyEnable==0 && UmiRpmhTouchVoteSettled(),
      [](unsigned pin,bool high){DrivePowerPin(pin,high);},
      [](){return unsigned(High(38)) | unsigned(High(69))<<1;},
      [](bool on){return on?UmiRpmhTouchEnable():UmiRpmhTouchDisable();},
      [](){return ReadSupplyState();},
      [](unsigned ms){MicroSecondDelay(ms*1000);});
    mFullCycleElapsedNs=GetTimeInNanoSecond(GetPerformanceCounter()-started);
    if(mFullCycle.result)return EFI_DEVICE_ERROR;
  } else if (mCycleEnabled) {
    mCycleRequested = TRUE;
    UINT64 started = GetPerformanceCounter ();
    mAvddCycle = umi_fts_avdd_cycle(
      [](unsigned pin, bool high){DrivePowerPin(pin, high);},
      [](){return unsigned(High(38)) | unsigned(High(69)) << 1;},
      [](unsigned ms){MicroSecondDelay(ms * 1000);});
    mCycleElapsedNs = GetTimeInNanoSecond(GetPerformanceCounter() - started);
    if (mAvddCycle.result) return EFI_DEVICE_ERROR;
  } else {
    // Normal native probe: AVDD enabled, RESET requested output-high, then
    // fts_init performs the first 10 ms GPIO reset. No supply OFF request.
    DrivePowerPin (69, TRUE);
    DrivePowerPin (38, TRUE);
    DrivePowerPin (38, FALSE);
    MicroSecondDelay (10000); // fts_system_reset: RESET low for 10 ms.
    DrivePowerPin (38, TRUE);
  }
  mAwaitReady = TRUE;
  mReadyStart = GetPerformanceCounter ();
  DEBUG ((DEBUG_WARN, "UMI touch power: L1 SID%u ready at 1800mV; AVDD high; reset released\n", mSupplySid));
  return PowerReady () ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS
ConfigureIrq (VOID)
{
  mSavedIrqCtl = MmioRead32 (GPIO_CTL (39));
  mActiveIrqCtl = mSavedIrqCtl & ~(GPIO_FUNC_MASK | GPIO_OE | GPIO_PULL_MASK | GPIO_DRIVE_MASK);
  // Official umi ts_active: GPIO input, pull-up, drive-strength 8mA.
  mActiveIrqCtl |= 3U | (3U << 6);
  if (mActiveIrqCtl & BIT11) mActiveIrqCtl |= BIT12;
  mIrqOwned = TRUE;
  MmioWrite32 (GPIO_CTL (39), mActiveIrqCtl);
  MemoryFence ();
  return MmioRead32 (GPIO_CTL (39)) == mActiveIrqCtl ? EFI_SUCCESS : EFI_WRITE_PROTECTED;
}

STATIC VOID
RestoreIrq (VOID)
{
  if (mIrqOwned) {
    MmioWrite32 (GPIO_CTL (39), mSavedIrqCtl);
    MemoryFence ();
    mIrqOwned = FALSE;
  }
}

STATIC VOID
ReleasePins (VOID)
{
  UINTN Index;
  UINT32 Pin;
  RestoreIrq ();
  if (!mOwned) {
    return;
  }
  /* First release both drivers, then restore the saved output latch and mux.
   * Only restore writable OUT bit 1, never replay sampled input/status bits. */
  for (Index = 0; Index < 2; Index++) {
    MmioWrite32 (GPIO_CTL (SDA + (UINT32)Index), mGpioCtl[Index] & ~GPIO_OE);
  }
  MemoryFence ();
  for (Index = 0; Index < 2; Index++) {
    Pin = SDA + (UINT32)Index;
    MmioWrite32 (GPIO_IO (Pin), (MmioRead32 (GPIO_IO (Pin)) & ~GPIO_OUT) | mSavedOut[Index]);
    MmioWrite32 (GPIO_CTL (Pin), mSavedCtl[Index]);
  }
  MemoryFence ();
  mOwned = FALSE;
}

STATIC EFI_STATUS
AcquirePins (VOID)
{
  UINTN Index;
  UINT32 Pin;
  UINT32 Ctl;
  if ((MmioRead32 (SE13_BASE + 0x40) & GENI_BUSY_MASK) != 0) {
    DEBUG ((DEBUG_WARN, "UMI touch preflight: GENI active\n"));
    return EFI_ACCESS_DENIED;
  }
  for (Index = 0; Index < 2; Index++) {
    Pin = SDA + (UINT32)Index;
    mSavedCtl[Index] = MmioRead32 (GPIO_CTL (Pin));
    mSavedOut[Index] = MmioRead32 (GPIO_IO (Pin)) & GPIO_OUT;
    Ctl = mSavedCtl[Index] & ~(GPIO_FUNC_MASK | GPIO_OE | GPIO_PULL_MASK | GPIO_DRIVE_MASK);
    if ((Ctl & BIT11) != 0) {
      Ctl |= BIT12; /* Linux pinctrl-msm: enable eGPIO if present. */
    }
    if (mNative) Ctl |= 1U << 2; // Kona GPIO36/37 function1 = qup13, 2mA, no pull.
    mGpioCtl[Index] = Ctl;
  }
  mOwned = TRUE;
  for (Index = 0; Index < 2; Index++) {
    Pin = SDA + (UINT32)Index;
    MmioWrite32 (GPIO_CTL (Pin), mGpioCtl[Index]); /* GPIO input: released. */
    MmioWrite32 (GPIO_IO (Pin), MmioRead32 (GPIO_IO (Pin)) & ~GPIO_OUT);
    if (MmioRead32 (GPIO_CTL (Pin)) != mGpioCtl[Index]) {
      DEBUG ((DEBUG_WARN, "UMI touch preflight: GPIO%u configuration readback failed\n", Pin));
      ReleasePins ();
      return EFI_WRITE_PROTECTED;
    }
  }
  MemoryFence ();
  EFI_STATUS IrqStatus = ConfigureIrq ();
  if (EFI_ERROR (IrqStatus)) { ReleasePins (); return IrqStatus; }
  { // Always reset to obtain a fresh READY event, including inherited rails.
    DEBUG ((DEBUG_WARN, "UMI touch preflight: initializing touch power/reset\n"));
    EFI_STATUS Status = InitializePower ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  MicroSecondDelay (HALF_CYCLE_US);
  if (!High (SDA) || !High (SCL)) {
    DEBUG ((DEBUG_WARN, "UMI touch preflight: released SDA=%u SCL=%u\n", High (SDA), High (SCL)));
    ReleasePins ();
    return EFI_NOT_READY; /* No recovery clocks, pull-up changes or power cycle. */
  }
  return EFI_SUCCESS;
}

STATIC VOID
Line (UINT32 Pin, BOOLEAN Released)
{
  UINTN Index = Pin - SDA;
  /* Output latch stays zero for the complete ownership interval. */
  MmioWrite32 (GPIO_CTL (Pin), mGpioCtl[Index] | (Released ? 0 : GPIO_OE));
  MemoryFence ();
}

STATIC BOOLEAN
ControlReady (VOID)
{
  return mOwned && mPowerOwned && mIrqOwned && PowerReady () &&
      MmioRead32 (GPIO_CTL (SDA)) == mGpioCtl[0] &&
      MmioRead32 (GPIO_CTL (SCL)) == mGpioCtl[1] &&
      MmioRead32 (GPIO_CTL (39)) == mActiveIrqCtl &&
      !(MmioRead32 (SE13_BASE + 0x40) & GENI_BUSY_MASK) &&
      High (SDA) && High (SCL);
}

STATIC EFI_STATUS
ResetController (VOID)
{
  // fts_fw_update resets again after CRC/identity checks, before sensing.
  // Reuse our acquired reset pin; do not cycle AVDD or write controller NVM.
  if (!ControlReady ()) {
    return EFI_ACCESS_DENIED;
  }
  DrivePowerPin (38, FALSE);
  if (High (38)) return EFI_WRITE_PROTECTED;
  MicroSecondDelay (10000);
  DrivePowerPin (38, TRUE);
  mReadyStart = GetPerformanceCounter ();
  mAwaitReady = TRUE;
  return PowerReady () ? EFI_SUCCESS : EFI_WRITE_PROTECTED;
}

STATIC BOOLEAN
Expired (VOID)
{
  return GetTimeInNanoSecond (GetPerformanceCounter () - mTransferStart) >= mTransferLimitNs;
}

STATIC EFI_STATUS
ClockHigh (VOID)
{
  UINT32 Wait;
  UINT64 Now = GetPerformanceCounter ();
  UINT64 Gap = GetTimeInNanoSecond (Now - mClockLast);
  if (Gap > mMaxGapNs) mMaxGapNs = Gap;
  mClockLast = Now;
  if (Expired ()) {
    return EFI_TIMEOUT;
  }
  Line (SCL, TRUE);
  for (Wait = 0; !High (SCL); Wait++) {
    if ((Wait >= STRETCH_LIMIT_US) || Expired ()) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
  }
  MicroSecondDelay (HALF_CYCLE_US);
  return EFI_SUCCESS;
}

STATIC VOID
ClockLow (VOID)
{
  Line (SCL, FALSE);
  MicroSecondDelay (HALF_CYCLE_US);
}

STATIC EFI_STATUS
Start (BOOLEAN Repeated)
{
  EFI_STATUS Status;
  Line (SDA, TRUE);
  MicroSecondDelay (HALF_CYCLE_US);
  Status = ClockHigh ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (!High (SDA)) {
    return Repeated ? EFI_PROTOCOL_ERROR : EFI_NOT_READY;
  }
  Line (SDA, FALSE);
  MicroSecondDelay (HALF_CYCLE_US);
  ClockLow ();
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Stop (VOID)
{
  EFI_STATUS Status;
  ClockLow ();
  Line (SDA, FALSE);
  MicroSecondDelay (HALF_CYCLE_US);
  Status = ClockHigh ();
  Line (SDA, TRUE);
  MicroSecondDelay (HALF_CYCLE_US);
  return Status;
}

STATIC EFI_STATUS
WriteByte (UINT8 Value)
{
  INTN Bit;
  EFI_STATUS Status;
  BOOLEAN Ack;
  for (Bit = 7; Bit >= 0; Bit--) {
    Line (SDA, ((Value >> Bit) & 1) != 0);
    MicroSecondDelay (HALF_CYCLE_US);
    Status = ClockHigh ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (((Value >> Bit) & 1) && !High (SDA)) {
      return EFI_PROTOCOL_ERROR; /* Released SDA unexpectedly held low. */
    }
    ClockLow ();
  }
  Line (SDA, TRUE);
  MicroSecondDelay (HALF_CYCLE_US);
  Status = ClockHigh ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Ack = !High (SDA);
  if (Ack && (mAckIndex < 32)) {
    mAckMask |= 1U << mAckIndex;
  }
  mAckIndex++;
  ClockLow ();
  return Ack ? EFI_SUCCESS : EFI_NO_RESPONSE;
}

STATIC EFI_STATUS
ReadByte (UINT8 *Value, BOOLEAN Last)
{
  UINTN Bit;
  EFI_STATUS Status;
  *Value = 0;
  Line (SDA, TRUE);
  for (Bit = 0; Bit < 8; Bit++) {
    Status = ClockHigh ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *Value = (UINT8)((*Value << 1) | (High (SDA) ? 1 : 0));
    ClockLow ();
  }
  Line (SDA, Last); /* ACK every byte except the final byte (NACK). */
  MicroSecondDelay (HALF_CYCLE_US);
  Status = ClockHigh ();
  ClockLow ();
  Line (SDA, TRUE);
  return Status;
}

STATIC EFI_STATUS
Transfer (CONST UINT8 *Write, UINTN WriteSize, UINT8 *Read, UINTN ReadSize)
{
#ifdef UMI_FTS_NATIVE_GPI
  if (mNative) {
    mLastFifoRead = Write && WriteSize == 1 && Write[0] == 0x86 && ReadSize == 8;
    mAckMask = 0; mLastMaxGapNs = 0; mLastStopResult = EFI_NOT_READY;
    uint64_t start = GetPerformanceCounter ();
    unsigned status = ControlReady () ? gpi::transfer(Write,WriteSize,Read,ReadSize) : EFI_ACCESS_DENIED;
    mLastElapsedNs = GetTimeInNanoSecond (GetPerformanceCounter () - start);
    mLastPhase = gpi::phase; mLastByte = 0; mLastGeni = MmioRead32(SE13_BASE+0x40);
    mLastLines = unsigned(High(SDA)) | (unsigned(High(SCL)) << 1);
    mLastPinsValid = PowerReady() && MmioRead32(GPIO_CTL(SDA))==mGpioCtl[0] && MmioRead32(GPIO_CTL(SCL))==mGpioCtl[1];
    // Native completed transactions imply address/data ACK, no bitbang byte mask.
    if(!status){mLastStopResult=EFI_SUCCESS;}
    return status;
  }
#endif
  EFI_STATUS Status;
  EFI_STATUS StopStatus;
  UINTN Index;
  mLastFifoRead = WriteSize == 1 && Write[0] == 0x86 && ReadSize == 8;
  mTransferLimitNs = TRANSFER_LIMIT_NS;
  mCurrentPhase = mLastPhase = 1;
  mCurrentByte = mLastByte = mLastLines = mLastPinsValid = 0;
  mLastStopResult = MAX_UINT32;
  mLastElapsedNs = mLastMaxGapNs = mMaxGapNs = 0;
  mLastGeni = MmioRead32 (SE13_BASE + 0x40);
  if (!mOwned || !PowerReady () ||
      ((mLastGeni & GENI_BUSY_MASK) != 0)) {
    return EFI_ACCESS_DENIED;
  }
  mTransferStart = GetPerformanceCounter ();
  mClockLast = mTransferStart;
  mAckMask = 0;
  mAckIndex = 0;
  mCurrentPhase = 2;Status = Start (FALSE);
  if (!EFI_ERROR (Status)) {
    mCurrentPhase = 3;
    Status = WriteByte (0x92); /* The only slave address used: 0x49, write. */
  }
  for (Index = 0; !EFI_ERROR (Status) && (Index < WriteSize); Index++) {
    mCurrentPhase = 4;mCurrentByte = (UINT32)Index;
    Status = WriteByte (Write[Index]);
  }
  if (!EFI_ERROR (Status) && (ReadSize != 0)) {
    mCurrentPhase = 5;
    Status = Start (TRUE);
    if (!EFI_ERROR (Status)) {
      mCurrentPhase = 6;
      Status = WriteByte (0x93);
    }
    for (Index = 0; !EFI_ERROR (Status) && (Index < ReadSize); Index++) {
      mCurrentPhase = 7;mCurrentByte = (UINT32)Index;
      Status = ReadByte (&Read[Index], Index + 1 == ReadSize);
    }
  }
  /* Even an expired transfer gets only one bounded STOP attempt. */
  mLastPhase = mCurrentPhase;mLastByte = mCurrentByte;
  mLastElapsedNs = GetTimeInNanoSecond (GetPerformanceCounter () - mTransferStart);
  mTransferStart = GetPerformanceCounter ();
  mTransferLimitNs = CLEANUP_LIMIT_NS;
  StopStatus = Stop ();
  mLastStopResult = (UINT32)StopStatus;
  mLastElapsedNs += GetTimeInNanoSecond (GetPerformanceCounter () - mTransferStart);
  mLastMaxGapNs = mMaxGapNs;
  mLastLines = (High (SDA) ? 1U : 0U) | (High (SCL) ? 2U : 0U);
  mLastPinsValid = PowerReady () &&
      MmioRead32 (GPIO_CTL (SDA)) == mGpioCtl[0] &&
      MmioRead32 (GPIO_CTL (SCL)) == mGpioCtl[1];
  mLastGeni = MmioRead32 (SE13_BASE + 0x40);
  if (!EFI_ERROR (Status)) {
    Status = StopStatus;
    if (EFI_ERROR (StopStatus)) mLastPhase = 8;
  }
  return Status;
}

#include "UmiFtsRecovery.inc"

