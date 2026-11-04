/* SPDX-License-Identifier: MIT */
/* A830Regs.h as the orchestrator relies on it: the header stands alone, compiles as C11 and C++17 (strict: -Wall -Wextra
 * -Werror -Wpedantic) with every one of its A830_STATIC_ASSERTs holding, and its facts agree with the orchestrator's own
 * headers (board, status block, start steps) and the values ARCHITECTURE 3.2-3.6, 4.6.2, 6.4 and 8.2 fix.
 *  1. This translation unit includes A830Regs.h first (no prerequisite header) in C++ (the runner's dialect); the header's
 *     static assertions are evaluated here, the cross-header ones below.
 *  2. At run time the header is compiled on its own as C11 and as C++17 with xcrun clang (the kernel builds it as gnu17 and
 *     gnu++2b; tests/test_a830regs.c checks the Mesa offsets at run time). Without a compiler the run reports SKIP for that
 *     part and fails nothing else.
 * host-test: A830Regs.h */
#include "A830Regs.h"
#include "XnuMemoryFixture.h"
#include "Adreno830Board.h"
#include "Adreno830Start.h"
#include "Adreno830Status.h"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern char **environ;

/* ---- Cross-header facts (compile time) ---------------------------------------------------------------------------- */
/* Windows of ARCHITECTURE 3.2 against the board defaults the device repository generates. */
static_assert(A830_GPU_PHYS==0x3D00000ull && A830_GPU_BYTES==0x40000u && A830_GMU_PHYS==0x3D37000ull && A830_GMU_BYTES==0x68000u &&
              A830_RSCC_PHYS==0x3D50000ull && A830_CX_MISC_PHYS==0x3D9E000ull && A830_SMMU_PHYS==0x3DA0000ull &&
              A830_GPUCC_PHYS==0x3D90000ull && A830_ZAP_PHYS==0x9B09A000ull && A830_ZAP_BYTES==0x2000u &&
              SM8750_CMD_DB_PHYS==0x81C60000ull && SM8750_SMEM_PHYS==0x81D00000ull && SM8750_QFPROM_PHYS==0x221C8000ull &&
              SM8750_RSC_DRV2_PHYS==0x16520000ull,"board windows (ARCHITECTURE 3.2)");
static_assert(A830_GMU_WIN_DELTA==A830_GMU_PHYS-A830_GPU_PHYS && A830_CX_MISC_WIN_DELTA==A830_CX_MISC_PHYS-A830_GPU_PHYS,
              "the orchestrator maps GPU-space dwords to the GMU and CX_MISC windows with these deltas");
static_assert(A830_GMU_REG_BYTE(A8XX_GMUCX_CM3_SYSRESET)==0x47000u,"S1 reads CM3_SYSRESET at GMU window +0x47000 (ARCH 0.3)");
static_assert(A830_GMU_REG_BYTE(A8XX_GMUAO_AHB_FENCE_STATUS)==0x57C4Cu,"the ring's fenced WPTR check (ARCH 4.13.2)");
static_assert(A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_SW_FUSE_VALUE)==0x1000u &&
              A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS)==0x1014u &&
              A830_CX_MISC_REG_BYTE(A8XX_CX_MISC_TCM_RET_CNTL)==0xE4u,"S2 fuse reads (ARCH 4.4.2)");
/* DeviceInfo of ARCHITECTURE 6.4 that S9 publishes. */
static_assert(A830_GMEM_BASE==0x4000000ull && A830_GMEM_BYTES==0xC00000u && A830_UCHE_TRAP_ADDR==0x1FFFFFFFFF000ull &&
              A830_GPU_VA_CLIENT_BASE==0x20000000ull && A830_GPU_VA_CLIENT_END_4G-A830_GPU_VA_CLIENT_BASE==0xE0000000ull &&
              A830_GPU_VA_CLIENT_END_2G-A830_GPU_VA_CLIENT_BASE==0x60000000ull && A830_UBWC_SWIZZLE==6u &&
              A830_UBWC_MACROTILE_MODE==1u && A830_UBWC_VERSION==5u && A830_HIGHEST_BANK_BIT==16u,"DeviceInfo (ARCH 6.4)");
/* The CB0 layout of ARCHITECTURE 4.6.2 (the kernel buffers S4 fills and S6 programs). */
static_assert(A830_GPU_VA_SQE==0x100000ull && A830_GPU_VA_SQE_BYTES==0x1C000u && A830_GPU_VA_AQE==0x180000ull &&
              A830_GPU_VA_AQE_BYTES==0x7000u && A830_GPU_VA_RING==0x200000ull && A830_RING_BYTES==0x8000u &&
              A830_GPU_VA_MEMPTRS==0x300000ull && A830_GPU_VA_PWRUP==0x301000ull && A830_GPU_VA_SCRATCH==0x302000ull,
              "CB0 layout (ARCH 4.6.2)");
/* Timeouts the orchestrator passes on (ARCHITECTURE 3.4 table). */
static_assert(A830_T_GX_ON_US==1000u && A830_T_CP_IDLE_US==2000000u && A830_T_FW_INIT_US==100000u && A830_T_OOB_US==100000u &&
              A830_T_HFI_ACK_US==1000000u && A830_T_SELFTEST_US==100000u && A830_T_SCM_US==1000000u,"stage wait limits");
/* B8 per profile: 100 ms designed (4.7.3), 500 ms annibale (kernel-sequences 5.2 step 4, section 14 row 22). */
static_assert(A830_T_HFI_INIT_US==100000u && A830_T_HFI_INIT_ANNIBALE_US==500000u,"B8 HFI init limits");
/* SCM, votes and SMMU values S2/S3 use. */
static_assert(A830_SCM_GPU_INIT_REQ==0x9u && A830_ZAP_PAS_ID==13u && A830_CN0_VOTE_ON==0x60000001u &&
              A830_DDR_BOOT_LEVEL==11u && A830_SMMU_GR0_S2CR(3)==0xC0Cu && A830_SMMU_S2CR_CHECK_MASK==0x030303FFu &&
              A830_SMMU_S2CR_TRANS_NSH(0)==0x300u && A830_SMMU_FSR_FAULT_MASK==0xC00001FEu && A830_SMMU_CB_FSR==0x58u &&
              A830_SMMU_CB_FAR==0x60u && A830_SMMU_CB_FAR_HI==0x64u,"S2/S3 constants");
/* The status block and the board structure of ARCHITECTURE 3.5 / 8.2. */
static_assert(sizeof(Adreno830Status)==576 && Adreno830StatusMagic==0x41383330u && Adreno830BoardMagic==0x41383342u &&
              A830SRunning==15 && A830StageCount==11 && A830ReasonCount==25,"status block and stages (ARCH 8.2, 8.3)");
static_assert(offsetof(Adreno830Status,hfi_late)==20 && offsetof(Adreno830Status,gmu_general11)==188,
              "the late HFI drain and GENERAL_11 in the former reserved words (ARCH 8.2)");
static_assert(Adreno830KnobCount==31 && Adreno830KnobTrace==25 && Adreno830KnobMaxLevel==26 && Adreno830KnobIrq==27 &&
              Adreno830KnobTtarget==28 && Adreno830KnobScreenOff==29 && Adreno830KnobKopt==30 && Adreno830LevelMax==16 &&
              Adreno830BinCount==6 && Adreno830LlccWindows==6,
              "board structure (ARCH 3.5, 3.6; the performance switches pagani-gpu-maxlevel and pagani-gpu-irq, the thermal "
              "loop's pagani-gpu-ttarget, the board-wide pagani-screenoff, then pagani-gpu-kopt last)");
static_assert(A830_GMU_INITIAL_LEVEL==2u && A830_GMU_COMMON_LEVELS==11u && A830_DDR_LEVELS==12u,
              "the default level table: GMU level 2 (222 MHz) of 11, 12 DDR levels (ARCH 4.1.4)");

/* ---- The header alone, as C11 and as C++17 (run time) ------------------------------------------------------------ */
static std::string directoryOf(const char *path)
{
    std::string text(path);
    const size_t slash=text.rfind('/');
    return slash==std::string::npos?std::string("."):text.substr(0,slash);
}
/* 0 compiled, 1 failed, -1 no compiler. */
static int compile(const std::string &header,const char *language,const char *standard)
{
    char unit[]="/tmp/test_a830regs_XXXXXX";
    const int fd=mkstemp(unit);
    if(fd<0)return -1;
    const std::string text="#include \""+header+"\"\nint a830_regs_unit_marker;\n";
    if(write(fd,text.data(),text.size())!=ssize_t(text.size())) {close(fd);unlink(unit);return -1;}
    close(fd);
    const char *argv[]={"xcrun","clang","-x",language,standard,"-Wall","-Wextra","-Werror","-Wpedantic","-fsyntax-only",unit,nullptr};
    pid_t pid=0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
    const int spawned=posix_spawnp(&pid,"xcrun",&actions,nullptr,const_cast<char *const *>(argv),environ);
    posix_spawn_file_actions_destroy(&actions);
    int status=0;
    const int result=spawned?-1:waitpid(pid,&status,0)<0?-1:WIFEXITED(status) && !WEXITSTATUS(status)?0:1;
    unlink(unit);
    return result;
}

int main()
{
    /* Spot values the header's own assertions do not pin, at run time too (so a header edit shows in the PASS line). */
    const struct {const char *name;uint64_t value,expected;} facts[]={
        {"A8XX_CP_ALWAYS_ON_COUNTER",A8XX_CP_ALWAYS_ON_COUNTER,0x8E7},{"A8XX_GMUCX_GFX_PWR_CLK_STATUS",A8XX_GMUCX_GFX_PWR_CLK_STATUS,0x1F7E8},
        {"A8XX_GMUCX_CM3_SYSRESET",A8XX_GMUCX_CM3_SYSRESET,0x1F800},{"A8XX_GMUAO_AHB_FENCE_STATUS",A8XX_GMUAO_AHB_FENCE_STATUS,0x23B13},
        {"A8XX_CP_HW_FAULT_STATUS_PIPE",A8XX_CP_HW_FAULT_STATUS_PIPE,0x8B3},{"A8XX_CP_PROTECT_STATUS_PIPE",A8XX_CP_PROTECT_STATUS_PIPE,0x84F},
        {"SM8750_GPUCC_PLL0_MODE",SM8750_GPUCC_PLL0_MODE,0},{"A830_AHB_FENCE_STATUS_CLR_VALUE",A830_AHB_FENCE_STATUS_CLR_VALUE,7}};
    unsigned spot=0;
    for(const auto &fact:facts) {
        if(fact.value!=fact.expected) {std::fprintf(stderr,"%s = %#llx, expected %#llx\n",fact.name,
                                                    (unsigned long long)fact.value,(unsigned long long)fact.expected);return 1;}
        ++spot;
    }
    const std::string header=directoryOf(__FILE__)+"/../A830Regs.h";
    const int c11=compile(header,"c","-std=c11"),cxx17=compile(header,"c++","-std=c++17");
    if(c11>0 || cxx17>0) {std::fprintf(stderr,"A830Regs.h alone: C11 %s, C++17 %s\n",c11?"failed":"ok",cxx17?"failed":"ok");return 1;}
    if(c11<0 || cxx17<0) {
        std::printf("PASS: A830Regs.h standalone in C++ with its static assertions, %u spot values and the cross-header facts; "
                    "SKIP the C11/C++17 compiles (no xcrun clang)\n",spot);
        return 0;
    }
    std::printf("PASS: A830Regs.h compiles alone as C11 and C++17 (-Wall -Wextra -Werror -Wpedantic) with its static assertions, "
                "%u spot values and the cross-header facts of the board, status block and stages hold\n",spot);
    return 0;
}
