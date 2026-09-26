/* MFRFLASH - MegaFlashROM SCC+ SD cached flash ROM loader v1.0
   By Konamiman 9/2026

   Flashes a ROM file into the Nextor area of a MegaFlashROM SCC+ SD
   cartridge. The entire file is first cached in RAM (the TPA plus as many
   mapped RAM segments as needed, from any mapper slot) and, if that is not
   enough, in the VRAM not used by the text screen. Only then, with
   interrupts disabled and without any further disk access, the flash is
   erased and programmed from the cache. This allows flashing the cartridge
   even when it is the active DOS controller (the one the system booted
   from): in that case the program hangs at the end and the computer must be
   reset.

   Compilation command line (see the Makefile in this directory):

   sdcc --code-loc 0x180 --data-loc 0 -mz80 --disable-warning 196 --disable-warning 85 --no-std-crt0
        -I<sdk>/C/includes -I<sdk>/C/code crt0_msxdos.rel asmcall.rel printf.rel print_msxdos.rel mfrflash.c
   objcopy -I ihex -O binary mfrflash.ihx MFRFLASH.COM

   Hardware notes (from the technical documentation of the cartridge by
   Manuel Pazos, as reproduced in the openMSX source code):

   - The flash chip is a Micron/Numonyx M29W640FB or M29W640GB (8 MB, used
     in 8 bit mode): AMD compatible command set, command addresses 4AAAh
     and 4555h relative to the start of page 1, DQ7/DQ5 status polling.
     Eight 8K blocks at the start of the chip, 64K blocks afterwards.
   - The Nextor kernel lives in the last 1 MB of the chip (offset 700000h),
     which is seen through subslot 3 of the cartridge as an ASCII8 mapper
     with the bank registers at 6000h/6800h/7000h/7800h. Bank values 40h-7Fh
     in the register at 6000h select the SD card registers instead of the
     flash; the same flash banks are reachable as C0h-FFh (the mirror).
   - Flash writes only reach the chip when bit 0 of the (write-only)
     configuration register at 7FFCh of subslot 1 is set; the register
     defaults to 03h at power-up and the original flashing tool (OPFXSD)
     writes 03h to it before flashing, so this tool does the same.
*/


/* Includes */

#include <stdio.h>
#include <string.h>
#include "asmcall.h"
#include "types.h"
#include "dos_functions.h"
#include "dos_errors.h"
#include "msx_bios.h"
#include "msx_workarea.h"
#include "driver_workarea.h"


/* Defines */

#define VERSION "1.0"

/* Flash geometry, relative to the Nextor area of the chip */
#define FLASH_AREA_SIZE   ((ulong)1024 * 1024)
#define FLASH_BLOCK_SIZE  ((ulong)64 * 1024)
#define FLASH_BANK_SIZE   0x2000
#define SD_REGISTERS_BANK 0x40   /* Banks 40h-7Fh in register 0 select the SD registers */
#define BANK_MIRROR_BIT   0x80
#define PROGRESS_CHUNK_SIZE 1024  /* One progress dot per chunk; must divide FLASH_BANK_SIZE */

/* Cartridge registers (all of them in page 1) */
#define BANK_REGISTER_0   ((byte*)0x6000)   /* 4000h-5FFFh, ASCII8 mapper of subslot 3 */
#define BANK_REGISTER_1   ((byte*)0x6800)   /* 6000h-7FFFh, idem */
#define CONFIG_REGISTER   ((byte*)0x7FFC)   /* In subslot 1 */
#define CONFIG_VALUE      0x03              /* Flash write enable + recovery blocks protect */
#define MAPPER_CHECK_SIZE 256               /* Bytes compared to verify the ASCII8 mapper */
#define NEXTOR_SUBSLOT_MASK 0x8C            /* Expanded slot, subslot 3 (FxxxSSPP format) */

/* Flash ID (autoselect mode): manufacturer and accepted device codes */
#define FLASH_MANUFACTURER_ST   0x20
#define FLASH_DEVICE_M29W640GB  0x7E
#define FLASH_DEVICE_M29W640FB  0xFD
#define FLASH_DEVICE_LEGACY     0x5B  /* Also accepted by OPFXSD */

/* Memory layout */
#define SEGMENT_SIZE    0x4000
#define PAGE1_ADDRESS   0x4000
#define PAGE2_ADDRESS   0x8000
#define MIN_TPA_TOP     0xC000  /* The stack must stay in page 3 */

/* System work area addresses not in the SDK headers */
#define RAMAD1  0xF342  /* Slot of the RAM in page 1 */
#define RAMAD2  0xF343  /* Slot of the RAM in page 2 */
#define MODE    0xFAFC  /* Bits 1-2: VRAM size (0=16K, 1=64K, 2=128K, 3=192K) */
#define TXTNAM  0xF3B3  /* Name table address for SCREEN 0 */
#define EXTBIO  0xFFCA  /* Extended BIOS entry point */
#define MSXVER  0x002D  /* MSX version byte in the main BIOS ROM */

/* Mapper support routine table offsets (DOS2-PIS section 5.2) */
#define MAP_ALL_SEG  0x00
#define MAP_FRE_SEG  0x03
#define MAP_GET_P2   0x27

/* Where the file is cached */
#define STORE_SEGMENT  0  /* A 16K mapped RAM segment (slot + segment number) */
#define STORE_VRAM     1  /* A 16K VRAM page (1-7) */
#define STORE_TPA      2  /* The free TPA area right after the program, up to 7FFFh */

#define MAX_STORES    66  /* 64 x 16K for a 1 MB file, plus the TPA area, plus one spare */
#define MAX_SEGMENTS  64

#define ROM_SIGNATURE_1 'A'
#define ROM_SIGNATURE_2 'B'

/* Handy macros */
#define DoDosCall(f) DosCall(f, &regs, REGS_ALL, REGS_ALL)
#define Min(a,b) ((a) < (b) ? (a) : (b))


/* Data types */

typedef struct {
    byte type;      /* STORE_* */
    byte slot;      /* STORE_SEGMENT: slot of the mapper */
    byte index;     /* STORE_SEGMENT: segment number, STORE_VRAM: 16K page (value for VDP R#14) */
    uint size;      /* Bytes of the file cached here */
} Store;


/* Strings */

const char* strTitle =
    "MegaFlashROM SCC+ SD cached flash ROM loader v" VERSION "\r\n"
    "(c) 2026 by Konamiman\r\n"
    "\r\n";

const char* strUsage =
    "Usage: MFRFLASH <file> <slot>[-<subslot>]|0 [/f] [/s]\r\n"
    "\r\n"
    "The entire file will be read and cached into RAM/VRAM before flashing.\r\n"
    "This allows to flash even slots corresponding to active DOS controllers\r\n"
    "(a reset will be needed afterwards in this case).\r\n"
    "\r\n"
    "Use 0 instead of a slot number to flash the primary DOS controller slot.\r\n"
    "Add /f to skip the MegaFlashROM SCC+ SD flash ROM detection.\r\n"
    "Add /s to skip all the confirmation prompts.\r\n";

const char* strCRLF = "\r\n";
const char* strOk = "OK\r\n";
const char* strError = "*** Error: ";
const char* strReset = "Reset your computer.\r\n";
const char* strInvParam = "Invalid parameter";
const char* strInvSlot = "Invalid slot";


/* Global variables */

Z80_registers regs;

char* fileName;
byte fileHandle;
ulong fileSize;
byte targetSlot;            /* FxxxSSPP, the subslot with the Nextor area */
bool forceFlag;             /* /f: skip the MegaFlashROM SCC+ SD detection */
bool skipConfirmations;     /* /s: don't ask for any confirmation */
bool isActiveController;    /* The target slot is a DOS controller in use */
bool interruptsDisabled;    /* Once set, console output goes through the BIOS */

uint mapperTable;           /* Mapper support routines jump table */
byte primaryMapperSlot;
byte ramSlot1;              /* Slot of the TPA RAM in page 1 */
byte ramSlot2;              /* Slot of the TPA RAM in page 2 */
byte tpaSegment2;           /* Segment of the TPA RAM in page 2 */
byte currentPage2Slot;
byte biosSlot;
byte msxVersion;

uint tpaAreaStart;          /* Free TPA area after the program: also the staging buffer */
uint tpaAreaSize;

Store stores[MAX_STORES];
byte storeCount;

byte allocatedSlots[MAX_SEGMENTS];    /* 0 = primary mapper, as FRE_SEG wants it */
byte allocatedSegments[MAX_SEGMENTS];
byte allocatedCount;

ulong progressBytes;
uint dotsPrinted;

/* Parameters and results of the assembly routines */
byte flashBank;
uint flashSource;
uint flashDestination;
uint flashLength;
byte flashResult;
byte flashIdManufacturer;
byte flashIdDevice;
byte segmentToSet;
byte vramPage;
uint vramOffset;
uint vramBuffer;
uint vramLength;
byte screenChar;
byte screenLine[80];

/* End of the program code and data, as laid out by the linker (crt0_msxdos.asm) */
extern byte HEAP_start;


/* Function prototypes */

void print(char* s);    /* From the SDK (print_msxdos.c) */

void Terminate(const char* errorMessage);
void TerminateWithDosError(byte errorCode);
void FlashError(const char* errorMessage);
void Print(const char* s);
void PrintCharDirect(char c);
void NewLineDirect();
void PrintProgress(uint bytes);
void CheckDosVersion();
void ExtractParameters(char** argv, int argc);
void ParseSlot(char* arg);
void GetSystemInfo();
bool ConfirmNonRomFile();
bool ConfirmFlashing();
bool AskYesNo();
void PrintSlot();
void CheckMegaFlashRom();
void PrepareCartridge();
void GetFileSize();
void PlanStorage();
bool AllocateSegment();
byte GetUsableVramPages();
void FreeAllocatedSegments();
void ReadFileIntoStores();
void ReadFromFile(uint address, uint length);
void SetPageSlot(byte page, byte slot);
void PageIntoPage2(byte slot, byte segment);
void RestoreTpaPage2();
void EraseFlash();
void ProgramStores();
void ProgramRange(ulong fileOffset, uint source, uint length);
byte BankValue(uint bank);
void CleanupAndTerminate();

void DisableInterrupts();
void EnableInterrupts();
void Hang();
void SelectFlashBank();
void FlashReadId();
void FlashEraseBank();
void FlashProgramAndVerify();
void ResetFlashBanks();
void SetPage2Segment();
void VramWrite();
void VramRead();


    /* MAIN */

int main(char** argv, int argc)
{
    Print(strTitle);

    if(argc == 0) {
        Print(strUsage);
        Terminate(null);
    }

    CheckDosVersion();
    ExtractParameters(argv, argc);
    GetSystemInfo();

    regs.Bytes.A = 0;    /* Open mode: read and write allowed (the file is only read) */
    regs.Words.DE = (int)fileName;
    DosCall(_OPEN, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Bytes.A != 0) {
        TerminateWithDosError(regs.Bytes.A);
    }
    fileHandle = regs.Bytes.B;

    if(!ConfirmNonRomFile()) {
        Terminate(null);
    }

    Print("Checking slot... ");
    if(forceFlag) {
        Print("skipped\r\n");
    } else {
        CheckMegaFlashRom();
        Print(strOk);
    }

    Print("Getting file size... ");
    GetFileSize();
    Print(strOk);

    Print("Allocating memory... ");
    PlanStorage();
    Print(strOk);

    Print("Reading file...\r\n");
    ReadFileIntoStores();
    Print(" OK\r\n");

    regs.Bytes.B = fileHandle;
    DosCall(_CLOSE, &regs, REGS_MAIN, REGS_NONE);

    if(!ConfirmFlashing()) {
        Terminate(null);
    }

    Print("\r\nNow flashing, DO NOT interrupt the program or reset/power down your computer!\r\n\r\n");

    /* Point of no return: from here on no DOS calls are made and interrupts
       stay disabled until the end (if the target slot is the active DOS
       controller, the kernel ROM is unusable while the flash is being
       erased or programmed, so no interrupt handler may run). */

    DisableInterrupts();
    interruptsDisabled = true;
    PrepareCartridge();

    Print("Deleting ROM... ");
    EraseFlash();
    Print(strOk);

    Print("Flashing ROM...\r\n");
    progressBytes = 0;
    dotsPrinted = 0;
    ProgramStores();
    Print(" OK\r\n");

    Print("\r\nFlashing completed.");
    if(isActiveController) {
        Print(" ");
        Print(strReset);
        Hang();
    }

    Print(strCRLF);
    CleanupAndTerminate();
    return 0;
}


/* Termination */

void Terminate(const char* errorMessage)
{
    if(errorMessage != null) {
        Print(strError);
        Print(errorMessage);
        Print(strCRLF);
    }

    FreeAllocatedSegments();

    regs.Bytes.B = (errorMessage == null ? 0 : 1);
    DosCall(_TERM, &regs, REGS_MAIN, REGS_NONE);
}


void TerminateWithDosError(byte errorCode)
{
    char message[64];

    regs.Bytes.B = errorCode;
    regs.Words.DE = (int)message;
    DosCall(_EXPLAIN, &regs, REGS_MAIN, REGS_NONE);
    Terminate(message);
}


/* Error while erasing or programming. If the target is the active DOS
   controller the computer probably can't boot from it anymore, and
   returning to DOS is not an option anyway: just hang. */
void FlashError(const char* errorMessage)
{
    Print(strError);
    Print(errorMessage);
    Print(strCRLF);

    if(isActiveController) {
        Print(strReset);
        Hang();
    }

    /* Leave the flash with the default banks (the failing routine has
       already reset it to read mode) and restore the paging before going
       back to DOS. */
    ResetFlashBanks();
    SetPageSlot(1, ramSlot1);
    RestoreTpaPage2();
    EnableInterrupts();
    FreeAllocatedSegments();

    regs.Bytes.B = 1;
    DosCall(_TERM, &regs, REGS_MAIN, REGS_NONE);
}


/* Normal termination after a successful flashing of a non-active slot. */
void CleanupAndTerminate()
{
    SetPageSlot(1, ramSlot1);
    RestoreTpaPage2();
    EnableInterrupts();
    Terminate(null);
}


/* Console output */

/* While interrupts are disabled neither DOS function calls nor the BIOS
   CHPUT routine can be used: both re-enable interrupts (CHPUT does it in
   its VRAM access routines), and an interrupt would be fatal when the
   slot being flashed is the active DOS controller, since the Nextor
   kernel hooks the timer interrupt from its (by then erased) ROM. So the
   text is written directly to the name table of the text screen, keeping
   the BIOS cursor variables up to date. */
void Print(const char* s)
{
    if(!interruptsDisabled) {
        print((char*)s);
        return;
    }

    while(*s) {
        PrintCharDirect(*s++);
    }
}


/* Prints one character in the text screen (SCREEN 0, 40 or 80 columns)
   by writing to VRAM directly. Handles CR, LF, wrapping at the screen
   width and scrolling, the way the BIOS does. */
void PrintCharDirect(char c)
{
    byte* csrX = (byte*)CSRX;
    byte* csrY = (byte*)CSRY;
    byte width = *(byte*)LINLEN;
    uint stride = width > 40 ? 80 : 40;

    if(c == '\r') {
        *csrX = 1;
        return;
    }

    if(c == '\n') {
        NewLineDirect();
        return;
    }

    /* The BIOS centers the text when the logical width is smaller than
       the physical one, e.g. WIDTH 37 on a 40 columns screen leaves a
       left margin of 2 columns (the odd column goes to the left). */
    screenChar = c;
    vramPage = 0;
    vramOffset = *(uint*)TXTNAM + (*csrY - 1) * stride + ((stride - width + 1) >> 1) + (*csrX - 1);
    vramBuffer = (uint)&screenChar;
    vramLength = 1;
    VramWrite();

    if(++(*csrX) > width) {
        *csrX = 1;
        NewLineDirect();
    }
}


void NewLineDirect()
{
    byte* csrY = (byte*)CSRY;
    byte rows = *(byte*)CRTCNT;
    uint stride = *(byte*)LINLEN > 40 ? 80 : 40;
    uint base = *(uint*)TXTNAM;
    byte row;

    if(*csrY < rows) {
        (*csrY)++;
        return;
    }

    /* Scroll the screen up one line */
    vramPage = 0;
    vramBuffer = (uint)screenLine;
    vramLength = stride;
    for(row = 1; row < rows; row++) {
        vramOffset = base + row * stride;
        VramRead();
        vramOffset -= stride;
        VramWrite();
    }
    memset(screenLine, ' ', stride);
    vramOffset = base + (rows - 1) * stride;
    VramWrite();
}


/* One dot per 1K of data processed. Dots are printed after each block of
   data is processed, so several dots may be printed at once when reading
   the file (flashing is done in 1K chunks, so there it's one at a time).
   Lines are wrapped by the console itself at the screen width. */
void PrintProgress(uint bytes)
{
    uint dotsNeeded;

    progressBytes += bytes;
    dotsNeeded = (uint)((progressBytes + 1023) >> 10);
    while(dotsPrinted < dotsNeeded) {
        Print(".");
        dotsPrinted++;
    }
}


/* Parameters and system information */

void CheckDosVersion()
{
    DoDosCall(_DOSVER);
    if(regs.Bytes.B < 2) {
        Terminate("This program requires MSX-DOS 2 or Nextor");
    }
}


void ExtractParameters(char** argv, int argc)
{
    int i;
    char* arg;

    if(argc < 2 || argc > 4) {
        Terminate(strInvParam);
    }

    fileName = argv[0];

    for(i = 2; i < argc; i++) {
        arg = argv[i];
        if(arg[0] != '/' || arg[1] == '\0' || arg[2] != '\0') {
            Terminate(strInvParam);
        }
        switch(arg[1] | 0x20) {
            case 'f':
                forceFlag = true;
                break;
            case 's':
                skipConfirmations = true;
                break;
            default:
                Terminate(strInvParam);
        }
    }

    ParseSlot(argv[1]);
}


/* Converts "<slot>", "<slot>-<subslot>" or "0" (primary DOS controller)
   into the FxxxSSPP format in targetSlot. */
void ParseSlot(char* arg)
{
    byte slot;
    byte* expTbl = (byte*)EXPTBL;

    if(arg[0] == '0' && arg[1] == '\0') {
        targetSlot = *(byte*)MASTER_SLOT;
        return;
    }

    if(arg[0] < '0' || arg[0] > '3') {
        Terminate(strInvSlot);
    }
    slot = arg[0] - '0';

    if(arg[1] == '\0') {
        /* No subslot given: subslot 0 if the slot is expanded */
        targetSlot = (expTbl[slot] & 0x80) ? (slot | 0x80) : slot;
        return;
    }

    if(arg[1] != '-' || arg[2] < '0' || arg[2] > '3' || arg[3] != '\0') {
        Terminate(strInvSlot);
    }

    if(!(expTbl[slot] & 0x80)) {
        Terminate("The specified slot is not expanded");
    }

    targetSlot = slot | ((arg[2] - '0') << 2) | 0x80;
}


void GetSystemInfo()
{
    byte i;
    byte* drvTbl = (byte*)DRVTBL;

    /* Mapper support routines and primary mapper slot (DOS2-PIS section 5.2) */

    regs.Bytes.A = 0;
    regs.Bytes.D = 4;
    regs.Bytes.E = 1;   /* Get mapper variable table: A = primary mapper slot */
    AsmCall(EXTBIO, &regs, REGS_MAIN, REGS_MAIN);
    primaryMapperSlot = regs.Bytes.A;

    regs.Bytes.A = 0;
    regs.Bytes.D = 4;
    regs.Bytes.E = 2;   /* Get mapper support routine address */
    AsmCall(EXTBIO, &regs, REGS_MAIN, REGS_MAIN);
    mapperTable = regs.UWords.HL;

    AsmCall(mapperTable + MAP_GET_P2, &regs, REGS_NONE, REGS_AF);
    tpaSegment2 = regs.Bytes.A;

    ramSlot1 = *(byte*)RAMAD1;
    ramSlot2 = *(byte*)RAMAD2;
    currentPage2Slot = ramSlot2;
    biosSlot = *(byte*)EXPTBL;

    /* MSX version (0 = MSX1), from the BIOS ROM. RDSLT leaves interrupts disabled. */

    regs.Bytes.A = biosSlot;
    regs.Words.HL = MSXVER;
    AsmCall(RDSLT, &regs, REGS_MAIN, REGS_AF);
    msxVersion = regs.Bytes.A;
    EnableInterrupts();

    /* Is the target slot a DOS controller in use? */

    isActiveController = (targetSlot == *(byte*)MASTER_SLOT);
    for(i = 0; i < 4; i++) {
        if(drvTbl[0] != 0 && drvTbl[1] == targetSlot) {
            isActiveController = true;
        }
        drvTbl += 2;
    }

    /* Free TPA area after the program, up to the end of page 1, 1K aligned.
       It's used as the staging buffer for the file reads, and (since it's
       filled last) also to cache the tail of the file. */

    tpaAreaStart = ((uint)&HEAP_start + 1023) & 0xFC00;
    tpaAreaSize = (PAGE2_ADDRESS - tpaAreaStart);

    /* The stack (at the end of the TPA) must be in page 3, since page 2
       gets remapped while caching and flashing. */

    if(*(uint*)0x0006 < MIN_TPA_TOP) {
        Terminate("Unsupported memory configuration (TPA end below C000h)");
    }
}


/* Confirmations (none is asked when /s is given) */

/* A ROM file starts with the "AB" signature. If the file doesn't,
   ask for confirmation before continuing. */
bool ConfirmNonRomFile()
{
    byte signature[2];

    regs.Bytes.B = fileHandle;
    regs.Words.DE = (int)signature;
    regs.Words.HL = 2;
    DosCall(_READ, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Bytes.A == 0 && regs.Words.HL == 2 &&
       signature[0] == ROM_SIGNATURE_1 && signature[1] == ROM_SIGNATURE_2) {
        return true;
    }

    Print("This file doesn't look like a ROM file (it doesn't start with \"AB\").\r\n");
    if(skipConfirmations) {
        return true;
    }

    Print("Flash it anyway? (Y/N) ");
    return AskYesNo();
}


/* Last chance to cancel, once the file is cached and before touching the
   flash. The actual slot is shown even if 0 was passed as the parameter. */
bool ConfirmFlashing()
{
    if(skipConfirmations) {
        return true;
    }

    Print("\r\nFile loaded in memory successfully.\r\n");
    if(isActiveController) {
        Print("Slot ");
        PrintSlot();
        Print(" is a DOS controller in use, you will need to reset your computer after flashing.\r\n");
    }
    Print("Are you sure that you want to flash it in slot ");
    PrintSlot();
    Print("? (Y/N) ");
    return AskYesNo();
}


/* Prints targetSlot as "<slot>" or "<slot>-<subslot>". */
void PrintSlot()
{
    if(targetSlot & 0x80) {
        printf("%u-%u", (uint)(targetSlot & 3), (uint)((targetSlot >> 2) & 3));
    } else {
        printf("%u", (uint)(targetSlot & 3));
    }
}


bool AskYesNo()
{
    char key;

    while(true) {
        regs.Bytes.A = 0;
        DosCall(_INNOE, &regs, REGS_MAIN, REGS_AF);
        key = regs.Bytes.A | 0x20;
        if(key == 'y' || key == 'n') {
            break;
        }
    }

    Print(key == 'y' ? "Y\r\n" : "N\r\n");
    return key == 'y';
}


/* Cartridge detection and preparation */

/* Verifies that the target slot is subslot 3 of an expanded slot (the
   cartridge always exposes its Nextor area there; the other subslots
   hold the recovery ROM, the game area and the RAM, all of which also
   answer to the flash ID query, and would get damaged) and that it
   contains a MegaFlashROM SCC+ SD flash chip (by reading its ID in
   autoselect mode) seen through an ASCII8 mapper (by checking that the
   bank register at 6000h changes what is visible at 4000h). */
void CheckMegaFlashRom()
{
    static byte bank0Contents[MAPPER_CHECK_SIZE];
    bool ok;

    if((targetSlot & NEXTOR_SUBSLOT_MASK) != NEXTOR_SUBSLOT_MASK) {
        Terminate("not subslot 3 (the Nextor area), use /f to override");
    }

    DisableInterrupts();
    SetPageSlot(1, targetSlot);

    flashBank = 0;
    SelectFlashBank();
    FlashReadId();
    ok = flashIdManufacturer == FLASH_MANUFACTURER_ST &&
        (flashIdDevice == FLASH_DEVICE_M29W640GB ||
         flashIdDevice == FLASH_DEVICE_M29W640FB ||
         flashIdDevice == FLASH_DEVICE_LEGACY);

    if(ok) {
        memcpy(bank0Contents, (byte*)PAGE1_ADDRESS, MAPPER_CHECK_SIZE);
        flashBank = 1;
        SelectFlashBank();
        ok = memcmp(bank0Contents, (byte*)PAGE1_ADDRESS, MAPPER_CHECK_SIZE) != 0;
        flashBank = 0;
        SelectFlashBank();
    }

    SetPageSlot(1, ramSlot1);
    EnableInterrupts();

    if(!ok) {
        Terminate("not a MegaFlashROM SCC+ SD (use /f to override)");
    }
}


/* Sets the configuration register of the cartridge (in subslot 1 of the
   same slot) to its default value, which enables flash writes. This is
   what OPFXSD does before flashing. Then leaves page 1 connected to the
   target slot, as the flashing routines expect. Interrupts must be disabled. */
void PrepareCartridge()
{
    if(targetSlot & 0x80) {
        SetPageSlot(1, (targetSlot & 0xF3) | (1 << 2));
        *CONFIG_REGISTER = CONFIG_VALUE;
    }

    SetPageSlot(1, targetSlot);
}


/* File size and cache planning */

void GetFileSize()
{
    regs.Bytes.B = fileHandle;
    regs.Bytes.A = 2;   /* Relative to the end of the file */
    regs.Words.HL = 0;
    regs.Words.DE = 0;
    DosCall(_SEEK, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Bytes.A != 0) {
        TerminateWithDosError(regs.Bytes.A);
    }
    fileSize = (ulong)regs.UWords.HL | ((ulong)regs.UWords.DE << 16);

    regs.Bytes.B = fileHandle;
    regs.Bytes.A = 0;   /* Back to the beginning */
    regs.Words.HL = 0;
    regs.Words.DE = 0;
    DosCall(_SEEK, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Bytes.A != 0) {
        TerminateWithDosError(regs.Bytes.A);
    }

    if(fileSize == 0) {
        Terminate("the file is empty");
    }
    if(fileSize > FLASH_AREA_SIZE) {
        Terminate("this file is bigger than the Nextor area of the flash ROM (1024K)");
    }
}


/* Decides where the file is cached, in this order: the TPA segment in page
   2, as many mapped RAM segments as can be allocated (primary mapper first,
   then any other mapper), the VRAM pages not used by the text screen, and
   finally the free TPA area after the program. Only what is needed for
   the file size is allocated. */
void PlanStorage()
{
    ulong capacity;
    ulong remaining;
    byte vramPages;
    byte i;
    Store* store;

    stores[0].type = STORE_SEGMENT;
    stores[0].slot = ramSlot2;
    stores[0].index = tpaSegment2;
    stores[0].size = SEGMENT_SIZE;
    storeCount = 1;
    capacity = SEGMENT_SIZE + tpaAreaSize;

    while(capacity < fileSize && AllocateSegment()) {
        capacity += SEGMENT_SIZE;
    }

    if(capacity < fileSize) {
        vramPages = GetUsableVramPages();
        for(i = 1; i <= vramPages && capacity < fileSize; i++) {
            store = &stores[storeCount++];
            store->type = STORE_VRAM;
            store->index = i;
            store->size = SEGMENT_SIZE;
            capacity += SEGMENT_SIZE;
        }
    }

    store = &stores[storeCount++];
    store->type = STORE_TPA;
    store->size = tpaAreaSize;

    if(capacity < fileSize) {
        printf("*** Error: not enough free memory to cache the file (%uK missing)\r\n",
            (uint)((fileSize - capacity + 1023) >> 10));
        Terminate(null);
    }

    /* Trim the stores to the file size */

    remaining = fileSize;
    for(i = 0; i < storeCount; i++) {
        if(remaining == 0) {
            storeCount = i;
            break;
        }
        store = &stores[i];
        if(store->size > remaining) {
            store->size = (uint)remaining;
        }
        remaining -= store->size;
    }
}


/* Allocates one user segment, from the primary mapper if possible and from
   any other mapper otherwise, and adds it as a store. */
bool AllocateSegment()
{
    byte slot;
    Store* store;

    if(allocatedCount >= MAX_SEGMENTS) {
        return false;
    }

    regs.Bytes.A = 0;   /* User segment */
    regs.Bytes.B = 0;   /* Primary mapper */
    AsmCall(mapperTable + MAP_ALL_SEG, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Flags.C) {
        regs.Bytes.A = 0;
        regs.Bytes.B = 0x20 | primaryMapperSlot;   /* Any mapper other than the primary one */
        AsmCall(mapperTable + MAP_ALL_SEG, &regs, REGS_MAIN, REGS_MAIN);
        if(regs.Flags.C) {
            return false;
        }
        slot = regs.Bytes.B;
        allocatedSlots[allocatedCount] = slot;
    } else {
        slot = primaryMapperSlot;
        allocatedSlots[allocatedCount] = 0;
    }
    allocatedSegments[allocatedCount] = regs.Bytes.A;
    allocatedCount++;

    store = &stores[storeCount++];
    store->type = STORE_SEGMENT;
    store->slot = slot;
    store->index = regs.Bytes.A;
    store->size = SEGMENT_SIZE;
    return true;
}


/* 16K VRAM pages usable as cache: all except the first one, which holds the
   text screen. Only for MSX2 or higher in a text screen mode (with the
   Kanji driver active the screen is a bitmap one that uses more VRAM). */
byte GetUsableVramPages()
{
    byte vramSize;

    if(msxVersion == 0 || *(byte*)SCRMOD > 1) {
        return 0;
    }

    vramSize = (*(byte*)MODE >> 1) & 3;
    if(vramSize == 0) {
        return 0;   /* 16K */
    }
    if(vramSize == 1) {
        return 3;   /* 64K */
    }
    return 7;       /* 128K, or 192K of which only 128K are addressable via R#14 */
}


void FreeAllocatedSegments()
{
    byte i;

    for(i = 0; i < allocatedCount; i++) {
        regs.Bytes.A = allocatedSegments[i];
        regs.Bytes.B = allocatedSlots[i];
        AsmCall(mapperTable + MAP_FRE_SEG, &regs, REGS_MAIN, REGS_NONE);
    }
    allocatedCount = 0;
}


/* Caching the file */

void ReadFileIntoStores()
{
    byte i;
    Store* store;
    uint offset;
    uint length;

    for(i = 0; i < storeCount; i++) {
        store = &stores[i];

        if(store->type == STORE_TPA) {
            /* Read directly into place */
            ReadFromFile(tpaAreaStart, store->size);
            PrintProgress(store->size);
            continue;
        }

        /* Read through the staging buffer, then copy to the destination
           with interrupts disabled (page 2 is temporarily remapped, and
           the VDP address must not be disturbed). */
        for(offset = 0; offset < store->size; offset += length) {
            length = Min(store->size - offset, tpaAreaSize);
            ReadFromFile(tpaAreaStart, length);

            DisableInterrupts();
            if(store->type == STORE_SEGMENT) {
                PageIntoPage2(store->slot, store->index);
                memcpy((byte*)(PAGE2_ADDRESS + offset), (byte*)tpaAreaStart, length);
                RestoreTpaPage2();
            } else {
                vramPage = store->index;
                vramOffset = offset;
                vramBuffer = tpaAreaStart;
                vramLength = length;
                VramWrite();
            }
            EnableInterrupts();

            PrintProgress(length);
        }
    }
}


/* Reads exactly the requested number of bytes. Disk errors are handled by
   DOS itself (Abort/Retry/Ignore prompt), any other error terminates. */
void ReadFromFile(uint address, uint length)
{
    regs.Bytes.B = fileHandle;
    regs.Words.DE = address;
    regs.Words.HL = length;
    DosCall(_READ, &regs, REGS_MAIN, REGS_MAIN);
    if(regs.Bytes.A != 0) {
        TerminateWithDosError(regs.Bytes.A);
    }
    if(regs.UWords.HL != length) {
        Terminate("unexpected end of file");
    }
}


/* Slot and segment paging */

/* Connects a slot to a page via the ENASLT routine entry in page 0.
   ENASLT leaves interrupts disabled. */
void SetPageSlot(byte page, byte slot)
{
    regs.Bytes.A = slot;
    regs.Bytes.H = page << 6;
    AsmCall(ENASLT, &regs, REGS_MAIN, REGS_NONE);
}


/* Makes a RAM segment of any mapper visible in page 2. The mapper
   registers are shared by all the mappers in the system, so the page 2
   register of the primary mapper changes too: RestoreTpaPage2 must be
   called before returning to DOS. Interrupts must be disabled. */
void PageIntoPage2(byte slot, byte segment)
{
    if(slot != currentPage2Slot) {
        SetPageSlot(2, slot);
        currentPage2Slot = slot;
    }
    segmentToSet = segment;
    SetPage2Segment();
}


void RestoreTpaPage2()
{
    PageIntoPage2(ramSlot2, tpaSegment2);
}


/* Flashing */

/* Bank register value for a given 8K bank of the Nextor area:
   banks 40h-7Fh must be selected through their mirror at C0h-FFh,
   since the direct values select the SD card registers instead. */
byte BankValue(uint bank)
{
    return (byte)(bank >= SD_REGISTERS_BANK ? bank | BANK_MIRROR_BIT : bank);
}


/* Erases only as many 64K blocks as the file needs. */
void EraseFlash()
{
    uint blocks;
    uint i;

    blocks = (uint)((fileSize + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE);
    for(i = 0; i < blocks; i++) {
        flashBank = BankValue(i * (FLASH_BLOCK_SIZE / FLASH_BANK_SIZE));
        SelectFlashBank();
        FlashEraseBank();
        if(flashResult != 0) {
            FlashError("erasing the flash ROM failed");
        }
    }
}


/* Programs the flash from the stores, in file order. Segments are paged
   into page 2 and programmed from there. VRAM pages and the TPA area are
   first copied to page 2 (whose TPA segment, being the first store, has
   already been flashed by then), 8K at a time, and programmed from there. */
void ProgramStores()
{
    byte i;
    Store* store;
    ulong fileOffset;
    uint offset;
    uint length;

    fileOffset = 0;
    for(i = 0; i < storeCount; i++) {
        store = &stores[i];

        if(store->type == STORE_SEGMENT) {
            PageIntoPage2(store->slot, store->index);
            ProgramRange(fileOffset, PAGE2_ADDRESS, store->size);
        } else {
            RestoreTpaPage2();
            for(offset = 0; offset < store->size; offset += length) {
                length = Min(store->size - offset, FLASH_BANK_SIZE);
                if(store->type == STORE_VRAM) {
                    vramPage = store->index;
                    vramOffset = offset;
                    vramBuffer = PAGE2_ADDRESS;
                    vramLength = length;
                    VramRead();
                } else {
                    SetPageSlot(1, ramSlot1);
                    memcpy((byte*)PAGE2_ADDRESS, (byte*)(tpaAreaStart + offset), length);
                    SetPageSlot(1, targetSlot);
                }
                ProgramRange(fileOffset + offset, PAGE2_ADDRESS, length);
            }
        }

        fileOffset += store->size;
    }

    ResetFlashBanks();
}


/* Programs (and verifies) a range of data, which may span several 8K
   banks, at the given offset of the Nextor area. The source must not be
   in page 1. It's done in chunks that don't cross a 1K boundary (and thus
   neither a bank boundary), so that each progress dot is printed as soon
   as its 1K of data has been flashed. */
void ProgramRange(ulong fileOffset, uint source, uint length)
{
    uint inBank;
    uint chunk;

    while(length > 0) {
        inBank = (uint)fileOffset & (FLASH_BANK_SIZE - 1);
        chunk = Min(length, PROGRESS_CHUNK_SIZE - (inBank & (PROGRESS_CHUNK_SIZE - 1)));

        flashBank = BankValue((uint)(fileOffset >> 13));
        SelectFlashBank();
        flashSource = source;
        flashDestination = PAGE1_ADDRESS + inBank;
        flashLength = chunk;
        FlashProgramAndVerify();
        if(flashResult == 1) {
            FlashError("programming the flash ROM failed");
        } else if(flashResult != 0) {
            FlashError("verification failed, the flash ROM contents don't match the file");
        }

        PrintProgress(chunk);
        fileOffset += chunk;
        source += chunk;
        length -= chunk;
    }
}


/* Assembly routines. Parameters and results are passed in global
   variables, so these don't depend on the SDCC calling convention. */

void DisableInterrupts() __naked
{
    __asm
    di
    ret
    __endasm;
}


void EnableInterrupts() __naked
{
    __asm
    ei
    ret
    __endasm;
}


void Hang() __naked
{
    __asm
hang_loop:
    di
    halt
    jr hang_loop
    __endasm;
}


/* Writes flashBank to the bank register for 4000h-5FFFh. Page 1 must be
   connected to the target slot. */
void SelectFlashBank() __naked
{
    __asm
    ld a,(_flashBank)
    ld (#0x6000),a
    ret
    __endasm;
}


/* Leaves the default banks (0 and 1) in the registers for 4000h-7FFFh. */
void ResetFlashBanks() __naked
{
    __asm
    xor a
    ld (#0x6000),a
    inc a
    ld (#0x6800),a
    ret
    __endasm;
}


/* Reads the flash ID (autoselect mode) into flashIdManufacturer and
   flashIdDevice. In 8 bit mode the device code is at (word) address 1,
   that is, byte address 2. */
void FlashReadId() __naked
{
    __asm
    ld a,#0xF0          ; Reset, just in case
    ld (#0x4000),a
    ld a,#0xAA
    ld (#0x4AAA),a
    ld a,#0x55
    ld (#0x4555),a
    ld a,#0x90          ; Autoselect
    ld (#0x4AAA),a
    ld a,(#0x4000)
    ld (_flashIdManufacturer),a
    ld a,(#0x4002)
    ld (_flashIdDevice),a
    ld a,#0xF0          ; Back to read mode
    ld (#0x4000),a
    ret
    __endasm;
}


/* Erases the block that contains the currently selected bank.
   flashResult: 0 = ok, 1 = error. */
void FlashEraseBank() __naked
{
    __asm
    ld a,#0xAA
    ld (#0x4AAA),a
    ld a,#0x55
    ld (#0x4555),a
    ld a,#0x80
    ld (#0x4AAA),a
    ld a,#0xAA
    ld (#0x4AAA),a
    ld a,#0x55
    ld (#0x4555),a
    ld a,#0x30          ; Block erase, at any address of the block
    ld hl,#0x4000
    ld (hl),a

    ; Data polling: DQ7 is 0 while erasing and 1 (the erased value)
    ; when done. DQ5 set means timeout, but DQ7 must be re-checked
    ; since both may change at the same time. The loop is also bounded
    ; (B:DE = 16 x 65536 iterations, over 10 seconds on a Z80, far
    ; beyond the erase time) in case the chip ignores the command,
    ; e.g. for a protected block, and just returns its old data.
    ld de,#0
    ld b,#16
erase_poll:
    ld a,(hl)
    rla
    jr c,erase_ok
    ld a,(hl)
    and #0x20
    jr nz,erase_check
    dec de
    ld a,d
    or e
    jr nz,erase_poll
    djnz erase_poll
    jr erase_error
erase_check:
    ld a,(hl)
    rla
    jr c,erase_ok

erase_error:
    ld a,#0xF0          ; Reset the chip
    ld (hl),a
    ld a,#1
    ld (_flashResult),a
    ret

erase_ok:
    xor a
    ld (_flashResult),a
    ret
    __endasm;
}


/* Programs flashLength bytes from flashSource (not in page 1) to
   flashDestination (in page 1, within the currently selected bank), one
   byte at a time with data polling, then verifies them by reading back.
   flashResult: 0 = ok, 1 = programming error, 2 = verification error. */
void FlashProgramAndVerify() __naked
{
    __asm
    ld hl,(_flashSource)
    ld de,(_flashDestination)
    ld bc,(_flashLength)

program_loop:
    ld a,#0xAA
    ld (#0x4AAA),a
    ld a,#0x55
    ld (#0x4555),a
    ld a,#0xA0          ; Program
    ld (#0x4AAA),a
    ld a,(hl)
    ld (de),a

    ; Data polling: DQ7 is the complement of the data bit 7 while
    ; programming. DQ5 set means timeout, but DQ7 must be re-checked
    ; since both may change at the same time. The loop is also bounded
    ; (65536 iterations, about a second on a Z80, far beyond the byte
    ; program time) in case the chip ignores the command, e.g. for a
    ; protected block, and just returns its old data.
    push bc
    ld bc,#0
program_poll:
    ld a,(de)
    xor (hl)
    jp p,program_done   ; Bit 7 matches: done
    ld a,(de)
    and #0x20
    jr nz,program_check
    dec bc
    ld a,b
    or c
    jr nz,program_poll
    jr program_error
program_check:
    ld a,(de)
    xor (hl)
    jp p,program_done

program_error:
    pop bc
    ld a,#0xF0          ; Reset the chip
    ld (#0x4000),a
    ld a,#1
    ld (_flashResult),a
    ret

program_done:
    pop bc
    inc hl
    inc de
    dec bc
    ld a,b
    or c
    jr nz,program_loop

    ; Verify
    ld hl,(_flashSource)
    ld de,(_flashDestination)
    ld bc,(_flashLength)
verify_loop:
    ld a,(de)
    cp (hl)
    jr nz,verify_error
    inc hl
    inc de
    dec bc
    ld a,b
    or c
    jr nz,verify_loop

    xor a
    ld (_flashResult),a
    ret

verify_error:
    ld a,#2
    ld (_flashResult),a
    ret
    __endasm;
}


/* Writes segmentToSet to the mapper register of page 2. */
void SetPage2Segment() __naked
{
    __asm
    ld a,(_segmentToSet)
    out (#0xFE),a
    ret
    __endasm;
}


/* Copies vramLength bytes from vramBuffer to VRAM page vramPage (16K
   pages, the value for VDP register 14), offset vramOffset. Interrupts
   are disabled and left disabled. Register 14 is set back to 0 at the
   end, as the BIOS text output expects; it doesn't exist on MSX1 (whose
   VDP would take it for register 6) so it isn't touched there, where only
   page 0 is ever used. The loop is slow enough for the VDP in any
   screen mode. */
void VramWrite() __naked
{
    __asm
    di
    ld a,(_msxVersion)
    or a
    jr z,vram_write_addr
    ld a,(_vramPage)
    out (#0x99),a
    ld a,#0x80+14
    out (#0x99),a
vram_write_addr:
    ld a,(_vramOffset)
    out (#0x99),a
    ld a,(_vramOffset+1)
    and #0x3F
    or #0x40            ; Write
    out (#0x99),a
    ld hl,(_vramBuffer)
    ld de,(_vramLength)
vram_write_loop:
    ld a,(hl)
    out (#0x98),a
    inc hl
    dec de
    ld a,d
    or e
    jr nz,vram_write_loop
    ld a,(_msxVersion)
    or a
    ret z
    xor a
    out (#0x99),a
    ld a,#0x80+14
    out (#0x99),a
    ret
    __endasm;
}


/* The reverse of VramWrite. */
void VramRead() __naked
{
    __asm
    di
    ld a,(_msxVersion)
    or a
    jr z,vram_read_addr
    ld a,(_vramPage)
    out (#0x99),a
    ld a,#0x80+14
    out (#0x99),a
vram_read_addr:
    ld a,(_vramOffset)
    out (#0x99),a
    ld a,(_vramOffset+1)
    and #0x3F           ; Read
    out (#0x99),a
    ld hl,(_vramBuffer)
    ld de,(_vramLength)
vram_read_loop:
    in a,(#0x98)
    ld (hl),a
    inc hl
    dec de
    ld a,d
    or e
    jr nz,vram_read_loop
    ld a,(_msxVersion)
    or a
    ret z
    xor a
    out (#0x99),a
    ld a,#0x80+14
    out (#0x99),a
    ret
    __endasm;
}

