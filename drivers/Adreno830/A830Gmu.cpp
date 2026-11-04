/* SPDX-License-Identifier: MIT */
#include "A830Gmu.h"
#include "A830Profile.h"
#include "Adreno830Status.h"
#include <IOKit/IOLib.h>
#include <string.h>

namespace {
/* Byte offsets inside the windows of A830GmuIo for GPU-space dwords (ARCHITECTURE 0.3). */
constexpr uint32_t gmuReg(uint32_t dword) {return A830_GMU_REG_BYTE(dword);}
constexpr uint32_t cxReg(uint32_t dword) {return A830_CX_MISC_REG_BYTE(dword);}
constexpr uint32_t gpuReg(uint32_t dword) {return A830_GPU_REG_BYTE(dword);}
constexpr uint32_t rsccReg(uint32_t dword) {return A830_RSCC_REG_BYTE(dword);}
/* The GX GDSC of gx_clkctl lies inside the gmu window. */
constexpr uint32_t GxGdscr=uint32_t(A830_GX_GDSCR_PHYS-A830_GMU_PHYS);
constexpr uint32_t GxCfgGdscr=uint32_t(A830_GX_CFG_GDSCR_PHYS-A830_GMU_PHYS);
static_assert(GxGdscr+4<=A830_GMU_BYTES && GxCfgGdscr==GxGdscr+4,"GX GDSC inside the gmu window");
static_assert(gmuReg(A8XX_GMUCX_CM3_SYSRESET)==0x47000 && gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO)==0x47648 &&
              gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR)==0x57C10 && gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS)==0x46FA0,
              "ARCHITECTURE 4.7.3/4.7.8 physical addresses");
static_assert(cxReg(A8XX_CX_MISC_AO_COUNTER_LO)==0x200 && cxReg(A8XX_CX_MISC_GFX_PWR_CLK_STATUS)==0x20C,"CX_MISC offsets");

/* GMU facts from A830Regs.h (section 21 and the GDSC bits of section 9):
 *   GxPowerDownComplete   GX CFG_GDSCR (0x3D68028) bit15 POWER_DOWN_COMPLETE, polled by force-off step 6 (ARCHITECTURE 4.7.9)
 *   LegacyInitDone        CM3_FW_INIT_RESULT of a core <= 0x20010004 (Linux a6xx_gmu_start: 0xBABEFACE)
 *   StatusLevelReadback   armcpu_spi_attach read back of a level SPI, Group1, enabled, priority 0xA0 (ARCHITECTURE 4.12.1),
 *   StatusLevelMask       compared under 0xFF07 */
enum : uint32_t {
    GxPowerDownComplete=SM8750_GDSC_CFG_POWER_DOWN_COMPLETE,
    LegacyInitDone=A830_GMU_LEGACY_INIT_DONE,
    StatusLevelReadback=A830_SPI_LEVEL_READBACK,
    StatusLevelMask=A830_SPI_READBACK_MASK,
    TimerSkewSamples=A830CxTimerSamples,
    EventStorm=1u<<0,
    FenceRetries=A830_WPTR_FENCE_RETRIES,
};

uint64_t physOf(unsigned window,uint32_t offset)
{
    static const uint64_t base[A830GmuWindows]={A830_GMU_PHYS,A830_GMU_AO_PHYS,A830_RSCC_PHYS,A830_GPU_PHYS,A830_CX_MISC_PHYS};
    return window<A830GmuWindows?base[window]+offset:offset;
}

/* B8's HFI_CTRL_STATUS limit of a profile (A830Regs.h section 17): 100 ms designed, 500 ms annibale. */
constexpr uint32_t hfiInitUs(bool annibale) {return annibale?A830_T_HFI_INIT_ANNIBALE_US:A830_T_HFI_INIT_US;}
static_assert(hfiInitUs(true)==uint32_t(A830GmuHfiInitAnnibaleUs) && hfiInitUs(false)==100000u,"B8 limits");

/* 32-bit store from a general register: never "str wzr" (ARCHITECTURE 4.5.2 keeps hypervisor-emulated blocks happy, and a
 * store of 0 stays a plain store everywhere else). */
inline void store32(volatile uint8_t *address,uint32_t value)
{
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("str %w0, [%1]"::"r"(value),"r"(address):"memory");
#else
    *reinterpret_cast<volatile uint32_t *>(address)=value;
#endif
}
inline void dsbSy()
{
#if defined(__arm64__) || defined(__aarch64__)
    __asm__ volatile("dsb sy":::"memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

bool mmioAt(void *cookie,unsigned window,uint32_t offset,volatile uint8_t **address)
{
    const A830GmuMmio *mmio=static_cast<const A830GmuMmio *>(cookie);
    if(!mmio || window>=A830GmuWindows || !mmio->base[window] || (offset&3) || offset>=mmio->bytes[window] ||
        mmio->bytes[window]-offset<4)return false;
    *address=mmio->base[window]+offset;
    return true;
}
int mmioRead(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    volatile uint8_t *address=nullptr;
    if(!value || !mmioAt(cookie,window,offset,&address))return 1;
    *value=*reinterpret_cast<volatile uint32_t *>(address);
    return 0;
}
int mmioWrite(void *cookie,unsigned window,uint32_t offset,uint32_t value)
{
    volatile uint8_t *address=nullptr;
    if(!mmioAt(cookie,window,offset,&address))return 1;
    store32(address,value);
    return 0;
}
void mmioBarrier(void *) {dsbSy();}
}

IOReturn A830GmuBindMmio(A830GmuMmio *mmio,A830GmuIo *io)
{
    if(!io)return kIOReturnBadArgument;
    *io={};
    if(!mmio || !mmio->base[A830GmuWindowGmu])return kIOReturnBadArgument;
    for(unsigned window=0;window<A830GmuWindows;++window)
        if(mmio->base[window] && (!mmio->bytes[window] || (mmio->bytes[window]&3) ||
            (reinterpret_cast<uintptr_t>(mmio->base[window])&3)))return kIOReturnBadArgument;
    *io={mmio,mmioRead,mmioWrite,mmioBarrier};
    return kIOReturnSuccess;
}

bool A830GmuDwordWindow(uint32_t dword,unsigned *window,uint32_t *offset)
{
    if(!window || !offset)return false;
    /* CX_MISC first: the end of the gmu window overlaps its first 0x1000 bytes. */
    if(A830_CX_MISC_REG_IN_WINDOW(dword)) {*window=A830GmuWindowCxMisc;*offset=cxReg(dword);return true;}
    if(A830_GPU_REG_IN_WINDOW(dword)) {*window=A830GmuWindowGpu;*offset=gpuReg(dword);return true;}
    if(A830_GMU_REG_IN_WINDOW(dword)) {*window=A830GmuWindowGmu;*offset=gmuReg(dword);return true;}
    return false;
}
int A830GmuReadDword(const A830GmuIo &io,uint32_t dword,uint32_t *value)
{
    unsigned window=0;uint32_t offset=0;
    if(!value)return 1;
    *value=0;
    if(!io.read32 || !A830GmuDwordWindow(dword,&window,&offset))return 1;
    return io.read32(io.cookie,window,offset,value);
}
int A830GmuWriteDword(const A830GmuIo &io,uint32_t dword,uint32_t value)
{
    unsigned window=0;uint32_t offset=0;
    if(!io.write32 || !A830GmuDwordWindow(dword,&window,&offset))return 1;
    return io.write32(io.cookie,window,offset,value);
}

/* --- CX timer (ARCHITECTURE 4.4.2) --------------------------------------------------------------------------------- */

IOReturn a830CxTimerSeed(A830CxMisc &cx)
{
    if(cx.seeded)return kIOReturnSuccess;
    if(!cx.io || !cx.io->write32 || !cx.os || !cx.os->counter || !cx.os->irq_disable || !cx.os->irq_restore)
        return kIOReturnBadArgument;
    /* annibale leaves AO_COUNTER_LO/HI alone (kernel-sequences section 14 row 21): the first round runs without the seed. */
    if(adreno830_annibale()) {++cx.skips;return kIOReturnSuccess;}
    const A830GmuIo &io=*cx.io;
    const A830GmuOs &os=*cx.os;
    const uint32_t low=cxReg(A8XX_CX_MISC_AO_COUNTER_LO),high=cxReg(A8XX_CX_MISC_AO_COUNTER_HI);
    bool ok=true;
    auto write=[&](uint64_t value) {
        ok=!io.write32(io.cookie,A830GmuWindowCxMisc,low,uint32_t(value)) && ok;
        ok=!io.write32(io.cookie,A830GmuWindowCxMisc,high,uint32_t(value>>32)) && ok;
        if(io.barrier)io.barrier(io.cookie);
    };
    /* The cost of the write, measured with interrupts off: the cost of two counter reads subtracted from the time a write
     * and its barrier take, averaged over 16 samples and rounded. */
    int64_t skew=0;
    uintptr_t state=os.irq_disable(os.cookie);
    for(uint32_t i=0;i<TimerSkewSamples;++i) {
        const uint64_t first=os.counter(os.cookie),second=os.counter(os.cookie);
        write(second);
        const uint64_t third=os.counter(os.cookie);
        skew+=int64_t(third-second)-int64_t(second-first);
    }
    os.irq_restore(os.cookie,state);
    if(skew<0)skew=0;
    const uint64_t average=(uint64_t(skew)+TimerSkewSamples/2)/TimerSkewSamples;
    state=os.irq_disable(os.cookie);
    const uint64_t seed=os.counter(os.cookie)+average;
    write(seed);
    os.irq_restore(os.cookie,state);
    if(!ok)return kIOReturnIOError;
    cx.seed=seed;cx.skew=average;cx.seeded=true;++cx.seeds;
    return kIOReturnSuccess;
}

void a830CxTimerLost(A830CxMisc &cx) {cx.seeded=false;}

IOReturn a830CxAhbTimeout(A830CxMisc &cx,uint32_t value)
{
    if(!cx.io || !cx.io->write32 || (value&~0x1FFFu))return kIOReturnBadArgument;
    const uint32_t registers[]={A8XX_CX_MISC_CX_AHB_AON_CNTL,A8XX_CX_MISC_CX_AHB_GMU_CNTL,A8XX_CX_MISC_CX_AHB_CP_CNTL,
                                A8XX_CX_MISC_CX_AHB_VBIF_SMMU_CNTL,A8XX_CX_MISC_CX_AHB_HOST_CNTL};
    cx.ahb_reg=0;cx.ahb_read=0;
    if(adreno830_annibale()) {
        /* annibale (K/adreno8_cx.c:647-662, measured on K90: all five read 0 at boot and 0x1B12 after the write): the five
         * values are read first (0xFFFFFFFF means no access), then each one not already holding the value is written,
         * completed and must read back exactly. */
        if(!cx.io->read32)return kIOReturnBadArgument;
        const A830GmuIo &io=*cx.io;
        auto refuse=[&cx](uint32_t dword,uint32_t read)->IOReturn {
            cx.ahb_reg=A830_CX_MISC_PHYS+cxReg(dword);cx.ahb_read=read;
            return kIOReturnIOError;
        };
        uint32_t inherited[sizeof registers/sizeof registers[0]]={};
        for(unsigned i=0;i<sizeof registers/sizeof registers[0];++i)
            if(io.read32(io.cookie,A830GmuWindowCxMisc,cxReg(registers[i]),&inherited[i]) || inherited[i]==0xffffffffu)
                return refuse(registers[i],0xffffffffu);
        for(unsigned i=0;i<sizeof registers/sizeof registers[0];++i) {
            if(inherited[i]==value)continue;
            if(io.write32(io.cookie,A830GmuWindowCxMisc,cxReg(registers[i]),value))return refuse(registers[i],0xffffffffu);
            if(io.barrier)io.barrier(io.cookie);
            uint32_t read=0;
            if(io.read32(io.cookie,A830GmuWindowCxMisc,cxReg(registers[i]),&read))return refuse(registers[i],0xffffffffu);
            if(read!=value)return refuse(registers[i],read);
        }
        return kIOReturnSuccess;
    }
    for(uint32_t dword:registers)
        if(cx.io->write32(cx.io->cookie,A830GmuWindowCxMisc,cxReg(dword),value))return kIOReturnIOError;
    if(cx.io->barrier)cx.io->barrier(cx.io->cookie);
    return kIOReturnSuccess;
}

/* --- configuration and helpers ---------------------------------------------------------------------------------- */

void A830GmuDefaultConfig(A830GmuConfig *config)
{
    if(!config)return;
    *config={};
    config->chip_id=A830_GMU_CHIPID;
    /* annibale's direct-ring (RB) value 1 << 31 | 0x32 << 18 | 0x8C0 (K/adreno8_gmu.c:607, kernel-sequences 4.6 step 15);
     * the design keeps KGSL's 0x8A0 lower bound. With IFPC off neither range is enforced. */
    config->ahb_fence_range=adreno830_annibale()?A830_AHB_FENCE_RANGE_LINUX:A830_AHB_FENCE_RANGE_KGSL;
    config->idle_level=A830_GMU_RPMH_POWER_STATE_ACTIVE;
    config->hfi_irq=336;
    config->gmu_irq=337;
    config->log_bytes=A830_GMU_LOG_BYTES;
    config->cx_busy_mask=A830_GMU_CX_BUSY_KGSL;
}

bool A830GmuInitDone(uint32_t core,uint32_t value)
{
    if(core<=A830_GMU_CORE_NEW_INIT_AFTER)return value==LegacyInitDone;
    return (value&A830_GMU_FW_INIT_MASK)==A830_GMU_FW_INIT_READY;
}

uint32_t A830GmuHfiInitUs()
{
    return hfiInitUs(adreno830_annibale()!=0);
}

int A830GmuGxVote(void *opaque,int on)
{
    A830GmuGxVoter *voter=static_cast<A830GmuGxVoter *>(opaque);
    if(!voter)return Sm8750RpmhArgument;
    if(!voter->drv2 || !voter->res || !voter->res->gfx.present)return voter->status=Sm8750RpmhArgument;
    const Sm8750RpmhArc &gfx=voter->res->gfx,&gmxc=voter->res->gmxc;
    uint32_t gfx_hlvl=0,gmxc_hlvl=0;
    if(on) {
        int status=Sm8750RpmhArcLevel(&gfx,1,&gfx_hlvl);
        if(!status && gmxc.present)status=Sm8750RpmhArcLevel(&gmxc,1,&gmxc_hlvl);
        if(status)return voter->status=status;
    }
    /* The memory rail of GX (MXC) leads when raising and follows when lowering, as mx before cx (ARCHITECTURE 4.3.2). */
    Sm8750RpmhCmd cmds[2]={};
    unsigned n=0;
    if(on && gmxc.present)cmds[n++]={gmxc.addr,gmxc_hlvl};
    cmds[n++]={gfx.addr,gfx_hlvl};
    if(!on && gmxc.present)cmds[n++]={gmxc.addr,0};
    const int status=Sm8750RpmhWriteActive(voter->drv2,cmds,n,(1u<<n)-1u,&voter->result);
    voter->status=status;
    if(!status) {voter->held=on;voter->gfx_hlvl=gfx_hlvl;voter->gmxc_hlvl=gmxc_hlvl;}
    return status;
}

IOReturn A830Gmu::fail(uint16_t step,uint16_t reason,uint64_t reg,uint64_t value,uint64_t expect,IOReturn result)
{
    failure_={step,reason,reg,value,expect};
    return result;
}

/* Warnings are also set from the top halves: always an atomic or. */
void A830Gmu::warn(uint32_t bits)
{
    __atomic_fetch_or(&report_.warnings,bits,__ATOMIC_RELAXED);
}

bool A830Gmu::rd(unsigned window,uint32_t offset,uint32_t *value)
{
    if(io_.read32(io_.cookie,window,offset,value)) {*value=0xffffffff;return false;}
    return true;
}

bool A830Gmu::wr(unsigned window,uint32_t offset,uint32_t value)
{
    return !io_.write32(io_.cookie,window,offset,value);
}

void A830Gmu::barrier()
{
    if(io_.barrier)io_.barrier(io_.cookie);
}

int A830Gmu::pollMask(unsigned window,uint32_t offset,uint32_t mask,uint32_t want,uint32_t timeout_us,uint32_t step_us,
    bool sleep,uint32_t *value,uint32_t *elapsed_us)
{
    const uint64_t start=os_.now_us(os_.cookie);
    for(;;) {
        uint32_t current=0;
        const bool read=rd(window,offset,&current);
        const uint64_t spent=os_.now_us(os_.cookie)-start;
        if(value)*value=current;
        if(elapsed_us)*elapsed_us=spent>UINT32_MAX?UINT32_MAX:uint32_t(spent);
        if(!read)return 2;
        if((current&mask)==want)return 0;
        if(spent>=timeout_us)return 1;
        if(sleep)os_.sleep_us(os_.cookie,step_us);else os_.delay_us(os_.cookie,step_us);
    }
}

/* The fatal GMU conditions ARCHITECTURE 10 keeps, as they show up while a sequence polls: GMU2HOST bit15 (CM3 fault) and AO
 * bit0 (watchdog bite), or a fatal bit a top half already took. bit23, FENCE_ERR and AHB errors are recorded only. */
bool A830Gmu::checkFatal(uint32_t *info,uint32_t *ao,uint16_t *reason)
{
    *info=0;*ao=0;
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO),info);
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS),ao);
    if(*info==0xffffffff)*info=0;
    if(*ao==0xffffffff)*ao=0;
    recordInterrupts(*info,*ao);
    if((*info&A830_GMU2HOST_CM3_FAULT) || (*ao&A830_GMU_AO_WDOG_BITE) || __atomic_load_n(&fatal_,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&fatal_,1u,__ATOMIC_RELEASE);
        *reason=A830ReasonGmuFault;
        return true;
    }
    return false;
}

void A830Gmu::recordInterrupts(uint32_t info,uint32_t ao)
{
    if(info)report_.info_last=info;
    if(ao) {
        report_.ao_last=ao;
        __atomic_store_n(&adreno830_status.sticky[3],ao,__ATOMIC_RELAXED);
    }
    if(info&A830_GMU2HOST_CM3_FAULT_LINUX)warn(A830GmuWarnCm3FaultLinux);
    if(ao&A830_GMU_AO_FENCE_ERR)warn(A830GmuWarnFenceErr);
    if(ao&A830_GMU_AO_HOST_AHB_BUS_ERR)warn(A830GmuWarnAhbBusErr);
}

IOReturn A830Gmu::bind(const A830GmuIo &io,const A830GmuOs &os,const A830GmuConfig &config,const A830GmuBlocks *firmware,
    NativeAdrenoGmuMemory *memory,const A830GmuHooks *hooks)
{
    if(bound_)return kIOReturnBusy;
    if(!io.read32 || !io.write32 || !os.now_us || !os.sleep_us || !os.delay_us)return kIOReturnBadArgument;
    if(!config.log_bytes || (config.log_bytes&0xfff) || config.log_bytes>0x100000)return kIOReturnBadArgument;
    if(config.cx_busy_mask && config.cx_busy_mask!=A830_GMU_CX_BUSY_KGSL && config.cx_busy_mask!=A830_GMU_CX_BUSY_LINUX)
        return kIOReturnBadArgument;
    io_=io;os_=os;config_=config;firmware_=firmware;memory_=memory;
    if(!config_.cx_busy_mask)config_.cx_busy_mask=A830_GMU_CX_BUSY_KGSL;
    hooks_=hooks?*hooks:A830GmuHooks{};
    report_={};failure_={};
    bound_=true;
    return kIOReturnSuccess;
}

/* --- cold boot B1-B8 (ARCHITECTURE 4.7.3) --------------------------------------------------------------------------- */

IOReturn A830Gmu::coldBoot(uint32_t *init_result)
{
    if(init_result)*init_result=0;
    if(!bound_)return kIOReturnNotReady;
    /* Read once: the whole boot follows one profile (A830Profile.h). */
    const bool annibale=adreno830_annibale();
    failure_={};
    ++report_.boot_attempts;
    booted_=false;
    oob_refs_[0]=oob_refs_[1]=0;
    __atomic_store_n(&fatal_,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&info_pending_,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&ao_pending_,0u,__ATOMIC_RELEASE);
    const uint32_t sysreset=gmuReg(A8XX_GMUCX_CM3_SYSRESET);
    const uint32_t info_reg=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO),ao_reg=gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS);
    auto access=[&](uint16_t step,unsigned window,uint32_t offset,uint32_t value) {
        return fail(step,A830ReasonReadback,physOf(window,offset),value,0,kIOReturnIOError);
    };

    /* B1: CM3 held in reset (it already is after power-up or forceOff; written anyway). */
    if(!wr(A830GmuWindowGmu,sysreset,A830_GMU_CM3_SYSRESET_ASSERT))return access(A830GmuStepB1,A830GmuWindowGmu,sysreset,1);
    barrier();

    /* B2: what the interrupt registers held is recorded, then everything is cleared before the first fault check, and only
     * bits 1, 2, 15, 23 (GMU2HOST) and 0, 3, 5 (AO) are unmasked. annibale opens none: both masks all ones first, then
     * both clears (kernel-sequences 4.6 steps 3-6, K/adreno8_gmu.c:576-580), and the fatal bits are polled. */
    (void)rd(A830GmuWindowGmu,info_reg,&report_.info_before);
    (void)rd(A830GmuWindowGmu,ao_reg,&report_.ao_before);
    const struct {uint32_t offset,value;} design_clear[]={
        {gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffff},{gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffff},
        {gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),A830_GMU2HOST_MASK_RUN},{gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),A830_GMU_AO_MASK_RUN}};
    const struct {uint32_t offset,value;} annibale_clear[]={
        {gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffff},{gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffff},
        {gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffff},{gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffff}};
    static_assert(sizeof design_clear==sizeof annibale_clear,"four B2 writes in both profiles");
    for(unsigned i=0;i<sizeof design_clear/sizeof design_clear[0];++i) {
        const uint32_t offset=annibale?annibale_clear[i].offset:design_clear[i].offset;
        const uint32_t value=annibale?annibale_clear[i].value:design_clear[i].value;
        if(!wr(A830GmuWindowGmu,offset,value))return access(A830GmuStepB2,A830GmuWindowGmu,offset,value);
    }
    hfi_idle_=ao_idle_=0;

    /* B3: RSCC wake after this driver's RSCC sleep. annibale also wakes an RSC it finds asleep at the cold boot,
     * RSC_STATUS0_DRV0 (0x3D50010) bit16 (kernel-sequences section 14 row 19; read in its stage 4, K/adreno8_cx.c:594,
     * acted on in stage 11 before the GMU registers are configured). */
    bool asleep=rscc_asleep_;
    if(annibale && rd(A830GmuWindowRscc,rsccReg(A8XX_RSCC_RSC_STATUS0_DRV0),&report_.rsc_status) &&
       (report_.rsc_status&A830_RSCC_SLEEP_DONE))asleep=true;
    if(asleep) {
        const IOReturn status=wakeRscc();
        if(status!=kIOReturnSuccess) {failure_.step=A830GmuStepB3;return status;}
    }

    /* B4: firmware into the TCMs (registers) and the DDR regions, then the ITCM path check (record only). annibale reads
     * every TCM word back and compares every DDR block (section 14 row 16): a difference fails B4. */
    if(!firmware_ || !memory_)return fail(A830GmuStepB4,A830ReasonFirmware,0,0,0,kIOReturnNotReady);
    {
        A830GmuLoadCheck check{};
        const IOReturn status=A830GmuFirmware::load(*firmware_,io_,*memory_,A830GmuLoadAll,&check);
        report_.tcm_verified=check.tcm_words;report_.ddr_verified=check.ddr_bytes;
        if(status!=kIOReturnSuccess && (check.mismatch || check.read_failed)) {
            const uint32_t offset=A830GmuFirmware::tcmOffset(check.addr);
            return fail(A830GmuStepB4,A830ReasonReadback,offset!=~0u?A830_GMU_PHYS+offset:uint64_t(check.addr),check.read,
                        check.expect,status);
        }
        if(status!=kIOReturnSuccess)
            return fail(A830GmuStepB4,status==kIOReturnIOError?A830ReasonReadback:A830ReasonFirmware,
                        A830_GMU_PHYS+gmuReg(A8XX_GMU_CM3_ITCM_START),firmware_->bytes,firmware_->end,status);
        if(A830GmuFirmware::checkItcm(*firmware_,io_,report_.itcm)!=kIOReturnSuccess)warn(A830GmuWarnItcm);
    }

    /* B5: version words (record only). The core version picks the B7 criterion: the image's own word when it has one (B4
     * wrote exactly that word into the DTCM, and a pinned image cannot change), else the word read back. */
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMU_DTCM_CORE_FW_VERSION),&report_.core);
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMU_DTCM_PWR_FW_VERSION),&report_.pwr);
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMU_DTCM_HFI_VERSION),&report_.hfi);
    if(((firmware_->versions&1) && report_.core!=firmware_->core) || ((firmware_->versions&2) && report_.pwr!=firmware_->pwr) ||
        ((firmware_->versions&4) && report_.hfi!=firmware_->hfi))warn(A830GmuWarnVersion);
    const uint32_t core=(firmware_->versions&1)?firmware_->core:report_.core;

    /* The HFI queues are reset while CM3 is still held in reset (ARCHITECTURE 4.7.5). */
    if(hooks_.reset_queues && hooks_.reset_queues(hooks_.cookie))
        return fail(A830GmuStepB6,A830ReasonArgument,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_HFI_QTBL_ADDR),0,0,kIOReturnNotReady);

    /* annibale, CM3 still in reset: GENERAL_11 (0x3D7E72C) holds the VRB address only in HWSCHED mode and must read 0 in
     * ordinary (RB) mode; annibale keeps CM3 in reset otherwise (K/adreno8_gmu.c:569-572, kernel-sequences section 14 row
     * 20, field-notes 5-5: 0 on K90). Recorded only here (a failed read records 0xFFFFFFFF). */
    if(annibale) {
        (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GENERAL_11),&report_.general11);
        if(report_.general11)warn(A830GmuWarnGeneral11);
    }

    /* B6: KGSL gen8_gmu_register_config. Nothing is read back except in diag mode, and then only recorded. */
    const uint64_t hfi_iova=memory_->ready()?memory_->layout().hfi_iova:uint64_t(A830_GMU_VA_HFI);
    const uint64_t log_iova=memory_->ready()?memory_->layout().log_iova:uint64_t(A830_GMU_VA_LOG);
    const struct {unsigned window;uint32_t dword,value;} config[]={
        {A830GmuWindowGmu,A8XX_GMUCX_RPMH_POWER_STATE,A830_GMU_RPMH_POWER_STATE_SLUMBER},
        {A830GmuWindowGmu,A8XX_GMUCX_CX_FALNEXT_INTF,A830_GMU_CX_FAL_VETO},
        {A830GmuWindowGmu,A8XX_GMUCX_CX_FAL_INTF,A830_GMU_CX_FAL_VETO},
        {A830GmuWindowGmu,A8XX_GMUCX_CM3_FW_INIT_RESULT,0},
        {A830GmuWindowGmu,A8XX_GMUCX_CM3_BOOT_CONFIG,A830_GMU_CM3_BOOT_CONFIG_VALUE},
        {A830GmuWindowGmu,A8XX_GMUCX_HFI_QTBL_ADDR,uint32_t(hfi_iova)},
        {A830GmuWindowGmu,A8XX_GMUCX_HFI_QTBL_INFO,A830_GMU_HFI_QTBL_INFO_VALUE},
        {A830GmuWindowGmu,A8XX_GMUAO_AHB_FENCE_RANGE_0,config_.ahb_fence_range},
        {A830GmuWindowGmu,A8XX_GMUCX_CM3_CFG,A830_GMU_CM3_CFG_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_QSB_SIDE(0),A830_GBIF_QSB_SIDE_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_QSB_SIDE(1),A830_GBIF_QSB_SIDE_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_QSB_SIDE(2),A830_GBIF_QSB_SIDE_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_QSB_SIDE(3),A830_GBIF_QSB_SIDE_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_CX_CONFIG,A830_GBIF_CX_CONFIG_VALUE},
        {A830GmuWindowGmu,A8XX_GMUCX_MRC_GBIF_QOS_CTRL,A830_GMU_MRC_GBIF_QOS_VALUE},
        {A830GmuWindowGpu,A8XX_GBIF_HALT,A830_GBIF_HALT_BOOT_VALUE},
        {A830GmuWindowGmu,A8XX_GMUCX_GENERAL_9,log_wptr_},
        {A830GmuWindowGmu,A8XX_GMUCX_GENERAL_10,config_.chip_id},
        {A830GmuWindowGmu,A8XX_GMUCX_GENERAL_8,A830_GMU_LOG_CFG(log_iova,config_.log_bytes)},
        {A830GmuWindowGmu,A8XX_GMUCX_SYS_BUS_CONFIG,A830_GMU_CACHE_CONFIG_VALUE},
        {A830GmuWindowGmu,A8XX_GMUCX_ICACHE_CONFIG,A830_GMU_CACHE_CONFIG_VALUE},
        {A830GmuWindowGmu,A8XX_GMUCX_DCACHE_CONFIG,A830_GMU_CACHE_CONFIG_VALUE},
    };
    for(const auto &entry:config) {
        const uint32_t offset=entry.window==A830GmuWindowGpu?gpuReg(entry.dword):gmuReg(entry.dword);
        if(!wr(entry.window,offset,entry.value))return access(A830GmuStepB6,entry.window,offset,entry.value);
    }
    barrier();
    report_.b6_mismatches=0;report_.b6_first=0;
    if(config_.diag)
        for(const auto &entry:config) {
            uint32_t value=0;
            const uint32_t offset=entry.window==A830GmuWindowGpu?gpuReg(entry.dword):gmuReg(entry.dword);
            if(!rd(entry.window,offset,&value) || value!=entry.value) {
                if(!report_.b6_mismatches++)report_.b6_first=entry.dword;
                warn(A830GmuWarnB6Readback);
            }
        }

    /* B7: release CM3 and wait for the firmware, sleeping 100 us per round, at most 100 ms. Fatal: 0xE00 in the result (NMI
     * or reset in progress), GMU2HOST bit15, AO bit0. */
    if(!wr(A830GmuWindowGmu,sysreset,A830_GMU_CM3_SYSRESET_RELEASE))return access(A830GmuStepB7,A830GmuWindowGmu,sysreset,0);
    barrier();
    const uint32_t init_reg=gmuReg(A8XX_GMUCX_CM3_FW_INIT_RESULT);
    const bool legacy=core<=A830_GMU_CORE_NEW_INIT_AFTER;
    const uint64_t begin=os_.now_us(os_.cookie);
    for(;;) {
        uint32_t value=0;
        const bool read=rd(A830GmuWindowGmu,init_reg,&value);
        const uint64_t spent=os_.now_us(os_.cookie)-begin;
        report_.init_result=value;
        report_.init_us=spent>UINT32_MAX?UINT32_MAX:uint32_t(spent);
        if(init_result)*init_result=value;
        if(!read)return access(A830GmuStepB7,A830GmuWindowGmu,init_reg,value);
        if(A830GmuInitDone(core,value))break;
        if(!legacy && (value&A830_GMU_FW_INIT_NMI_MASK))
            return fail(A830GmuStepB7,A830ReasonGmuNmi,A830_GMU_PHYS+init_reg,value,A830_GMU_FW_INIT_READY,kIOReturnIOError);
        uint32_t info=0,ao=0;uint16_t reason=0;
        if(checkFatal(&info,&ao,&reason)) {
            const bool cm3=info&A830_GMU2HOST_CM3_FAULT;
            return fail(A830GmuStepB7,reason,A830_GMU_PHYS+(cm3?info_reg:ao_reg),cm3?info:ao,0,kIOReturnIOError);
        }
        if(spent>=A830_T_FW_INIT_US)
            return fail(A830GmuStepB7,A830ReasonTimeout,A830_GMU_PHYS+init_reg,value,legacy?LegacyInitDone:A830_GMU_FW_INIT_READY,
                        kIOReturnTimeout);
        os_.sleep_us(os_.cookie,A830_POLL_STEP_US);
    }

    /* B8: HFI ready. annibale clears GMU2HOST and keeps it masked first (kernel-sequences 5.2 step 2, K/adreno8_hfi.c:342-343)
     * and waits 500 ms instead of 100 ms (5.2 step 4, section 14 row 22). */
    const uint32_t hfi_init=gmuReg(A8XX_GMUCX_HFI_CTRL_INIT),hfi_status=gmuReg(A8XX_GMUCX_HFI_CTRL_STATUS);
    if(annibale) {
        const uint32_t clear=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),mask=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK);
        if(!wr(A830GmuWindowGmu,clear,0xffffffff))return access(A830GmuStepB8,A830GmuWindowGmu,clear,0xffffffff);
        if(!wr(A830GmuWindowGmu,mask,0xffffffff))return access(A830GmuStepB8,A830GmuWindowGmu,mask,0xffffffff);
    }
    const uint32_t hfi_limit=hfiInitUs(annibale);   /* the boot's profile, read once above */
    if(!wr(A830GmuWindowGmu,hfi_init,A830_GMU_HFI_CTRL_INIT_VALUE))
        return access(A830GmuStepB8,A830GmuWindowGmu,hfi_init,A830_GMU_HFI_CTRL_INIT_VALUE);
    barrier();
    const uint64_t hfi_begin=os_.now_us(os_.cookie);
    for(;;) {
        uint32_t value=0;
        const bool read=rd(A830GmuWindowGmu,hfi_status,&value);
        const uint64_t spent=os_.now_us(os_.cookie)-hfi_begin;
        report_.hfi_status=value;
        report_.hfi_us=spent>UINT32_MAX?UINT32_MAX:uint32_t(spent);
        if(!read)return access(A830GmuStepB8,A830GmuWindowGmu,hfi_status,value);
        if(value&A830_GMU_HFI_CTRL_STATUS_READY)break;
        uint32_t info=0,ao=0;uint16_t reason=0;
        if(checkFatal(&info,&ao,&reason)) {
            const bool cm3=info&A830_GMU2HOST_CM3_FAULT;
            return fail(A830GmuStepB8,reason,A830_GMU_PHYS+(cm3?info_reg:ao_reg),cm3?info:ao,0,kIOReturnIOError);
        }
        if(spent>=hfi_limit)
            return fail(A830GmuStepB8,A830ReasonTimeout,A830_GMU_PHYS+hfi_status,value,A830_GMU_HFI_CTRL_STATUS_READY,
                        kIOReturnTimeout);
        os_.sleep_us(os_.cookie,A830_POLL_STEP_US);
    }
    booted_=true;first_boot_done_=true;
    ++report_.boots;
    return kIOReturnSuccess;
}

/* --- OOB and GX (ARCHITECTURE 4.7.8) ----------------------------------------------------------------------------------- */

IOReturn A830Gmu::oobSet(uint32_t *ack_us,uint32_t request)
{
    if(ack_us)*ack_us=0;
    if(!bound_)return kIOReturnNotReady;
    /* gen8 knows two OOB requests: GPU and perfcounter (KGSL refuses boot_slumber and dcvs, req >= 6 on gen8). */
    if(request>A830_OOB_REQ_PERFCNTR)return fail(A830GmuStepOob,A830ReasonArgument,0,request,A830_OOB_REQ_PERFCNTR,kIOReturnUnsupported);
    if(oob_refs_[request]) {++oob_refs_[request];return kIOReturnSuccess;}
    const uint32_t info_reg=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO),clear_reg=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR);
    const uint32_t set=A830_OOB_SET(request),ack=A830_OOB_ACK(request);
    /* An acknowledgement left from before would end the poll at once: recorded and cleared first. */
    uint32_t before=0;
    if(rd(A830GmuWindowGmu,info_reg,&before) && (before&ack)) {
        warn(A830GmuWarnOobResidue);
        (void)wr(A830GmuWindowGmu,clear_reg,ack);
    }
    if(!wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_HOST2GMU_INTR_SET),set))
        return fail(A830GmuStepOob,A830ReasonReadback,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_HOST2GMU_INTR_SET),set,0,kIOReturnIOError);
    barrier();
    uint32_t value=0,elapsed=0;
    const int poll=pollMask(A830GmuWindowGmu,info_reg,ack,ack,A830_T_OOB_US,A830_POLL_STEP_US,true,&value,&elapsed);
    (void)wr(A830GmuWindowGmu,clear_reg,ack);
    barrier();
    if(poll==2)return fail(A830GmuStepOob,A830ReasonReadback,A830_GMU_PHYS+info_reg,value,ack,kIOReturnIOError);
    if(poll==1)return fail(A830GmuStepOob,A830ReasonTimeout,A830_GMU_PHYS+info_reg,value,ack,kIOReturnTimeout);
    ++oob_refs_[request];
    if(request==A830_OOB_REQ_GPU)report_.oob_us=elapsed;
    if(ack_us)*ack_us=elapsed;
    return kIOReturnSuccess;
}

void A830Gmu::oobClear(uint32_t request)
{
    if(!bound_ || request>A830_OOB_REQ_PERFCNTR || !oob_refs_[request])return;
    if(--oob_refs_[request])return;
    (void)wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_HOST2GMU_INTR_SET),A830_OOB_CLEAR(request));
    barrier();
}

IOReturn A830Gmu::waitGxOn(uint32_t timeout_us)
{
    if(!bound_)return kIOReturnNotReady;
    const uint32_t status=gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS);
    uint32_t value=0,elapsed=0;
    const int poll=pollMask(A830GmuWindowGmu,status,A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF,0,timeout_us,A830_T_GX_ON_STEP_US,false,
                            &value,&elapsed);
    report_.gx_pwrclk=value;report_.gx_us=elapsed;
    /* Recorded only: RPMH_POWER_STATE (0 = active expected) and CX_MISC's own view, which no driver uses as a gate on A830. */
    (void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_RPMH_POWER_STATE),&report_.rpmh_state);
    if(report_.rpmh_state!=A830_GMU_RPMH_POWER_STATE_ACTIVE)warn(A830GmuWarnRpmhState);
    if(!rd(A830GmuWindowCxMisc,cxReg(A8XX_CX_MISC_GFX_PWR_CLK_STATUS),&report_.cx_gx_status))report_.cx_gx_status=0xffffffff;
    if(poll==0)return kIOReturnSuccess;
    /* GX is not powered: S5 stops here without touching a GX register (S6 would take a synchronous external abort). */
    return fail(A830GmuStepGxOn,poll==2?A830ReasonReadback:A830ReasonUnexpectedState,A830_GMU_PHYS+status,value,0,
                poll==2?kIOReturnIOError:kIOReturnTimeout);
}

IOReturn A830Gmu::writeCounters()
{
    if(!bound_)return kIOReturnNotReady;
    auto access=[&](uint32_t offset,uint32_t value) {
        return fail(A830GmuStepCounters,A830ReasonReadback,A830_GMU_PHYS+offset,value,0,kIOReturnIOError);
    };
    const uint32_t busy_mask=gmuReg(A8XX_GMUAO_GPU_CX_BUSY_MASK);
    if(!wr(A830GmuWindowGmu,busy_mask,A830_GMU_CX_BUSY_MASK_VALUE))return access(busy_mask,A830_GMU_CX_BUSY_MASK_VALUE);
    /* XOCLK_0 [7:0] |= 0x20 (GPU busy, the DCVS counter), XOCLK_1 [7:0] = 0x04 (IFPC residency); the upper selectors (BCL)
     * are kept. */
    const uint32_t select0=gmuReg(A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_0),select1=gmuReg(A8XX_GMUCX_POWER_COUNTER_SELECT_XOCLK_1);
    uint32_t value=0;
    if(!rd(A830GmuWindowGmu,select0,&value))return access(select0,value);
    value|=A830_GMU_PWRCNT_XOCLK0_GPU_BUSY;
    if(!wr(A830GmuWindowGmu,select0,value))return access(select0,value);
    if(!rd(A830GmuWindowGmu,select1,&value))return access(select1,value);
    value=(value&~A830_GMU_PWRCNT_SELECT_MASK)|A830_GMU_PWRCNT_XOCLK1_IFPC;
    if(!wr(A830GmuWindowGmu,select1,value))return access(select1,value);
    const struct {uint32_t offset,value;} writes[]={
        {gmuReg(A8XX_GMUCX_POWER_COUNTER_ENABLE),A830_GMU_POWER_COUNTER_ENABLE_VALUE},
        {gmuReg(A8XX_GMUAO_CGC_MODE_CNTL),A830_GMU_CGC_MODE_VALUE},
        {gmuReg(A8XX_GMUAO_CGC_DELAY_CNTL),A830_GMU_CGC_DELAY_VALUE},
        {gmuReg(A8XX_GMUAO_CGC_HYST_CNTL),A830_GMU_CGC_HYST_VALUE}};
    for(const auto &entry:writes)if(!wr(A830GmuWindowGmu,entry.offset,entry.value))return access(entry.offset,entry.value);
    barrier();
    return kIOReturnSuccess;
}

IOReturn A830Gmu::readBusy(uint64_t *ticks)
{
    if(!ticks)return kIOReturnBadArgument;
    *ticks=0;
    if(!bound_)return kIOReturnNotReady;
    const uint32_t low=gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_L_0),high=gmuReg(A8XX_GMUCX_POWER_COUNTER_XOCLK_H_0);
    uint32_t first=0,low_word=0,second=0;
    if(!rd(A830GmuWindowGmu,high,&first) || !rd(A830GmuWindowGmu,low,&low_word) || !rd(A830GmuWindowGmu,high,&second))
        return kIOReturnIOError;
    if(first!=second && !rd(A830GmuWindowGmu,low,&low_word))return kIOReturnIOError;
    *ticks=uint64_t(second)<<32|low_word;
    return kIOReturnSuccess;
}

/* --- RSCC (ARCHITECTURE 4.7.4, B3) ----------------------------------------------------------------------------------- */

IOReturn A830Gmu::rsccSleep()
{
    if(!bound_)return kIOReturnNotReady;
    /* KGSL gen8_rscc_sleep_sequence: nothing to do before the first boot completed, or twice. */
    if(!first_boot_done_ || rscc_asleep_)return kIOReturnSuccess;
    const uint32_t request=gmuReg(A8XX_GMUAO_RSCC_CONTROL_REQ);
    if(!wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_CM3_SYSRESET),A830_GMU_CM3_SYSRESET_ASSERT))
        return fail(A830GmuStepRsccSleep,A830ReasonReadback,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_CM3_SYSRESET),1,0,kIOReturnIOError);
    barrier();
    booted_=false;
    uint32_t wptr=0;
    if(rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GENERAL_9),&wptr)) {log_wptr_=wptr;report_.log_wptr=wptr;}
    if(!wr(A830GmuWindowGmu,request,A830_GMU_RSCC_CONTROL_SLEEP))
        return fail(A830GmuStepRsccSleep,A830ReasonReadback,A830_GMU_PHYS+request,A830_GMU_RSCC_CONTROL_SLEEP,0,kIOReturnIOError);
    barrier();
    uint32_t value=0;
    const uint32_t status=rsccReg(A8XX_RSCC_RSC_STATUS0_DRV0);
    const int poll=pollMask(A830GmuWindowRscc,status,A830_RSCC_SLEEP_DONE,A830_RSCC_SLEEP_DONE,A830_T_RSCC_US,A830_POLL_STEP_US,
                            true,&value,nullptr);
    if(poll)return fail(A830GmuStepRsccSleep,poll==2?A830ReasonReadback:A830ReasonTimeout,A830_RSCC_PHYS+status,value,
                        A830_RSCC_SLEEP_DONE,poll==2?kIOReturnIOError:kIOReturnTimeout);
    (void)wr(A830GmuWindowGmu,request,0);
    barrier();
    rscc_asleep_=true;
    ++report_.rscc_sleeps;
    return kIOReturnSuccess;
}

IOReturn A830Gmu::rsccWake()
{
    if(!bound_)return kIOReturnNotReady;
    if(!rscc_asleep_)return kIOReturnSuccess;
    return wakeRscc();
}

/* The wake handshake itself (KGSL gen8_rscc_wakeup_sequence). annibale ORs BIT1 into the request it finds and writes that
 * request back afterwards only when BIT1 was its own (K/adreno8_gmu.c:582-597); the design writes BIT1, then 0. */
IOReturn A830Gmu::wakeRscc()
{
    const uint32_t request=gmuReg(A8XX_GMUAO_RSCC_CONTROL_REQ),ack=gmuReg(A8XX_GMUAO_RSCC_CONTROL_ACK);
    const bool annibale=adreno830_annibale();
    uint32_t before=0;
    if(annibale && !rd(A830GmuWindowGmu,request,&before))
        return fail(A830GmuStepRsccWake,A830ReasonReadback,A830_GMU_PHYS+request,before,0,kIOReturnIOError);
    const uint32_t wake=annibale?before|A830_GMU_RSCC_CONTROL_WAKE:uint32_t(A830_GMU_RSCC_CONTROL_WAKE);
    if(!wr(A830GmuWindowGmu,request,wake))
        return fail(A830GmuStepRsccWake,A830ReasonReadback,A830_GMU_PHYS+request,wake,0,kIOReturnIOError);
    barrier();
    uint32_t value=0;
    int poll=pollMask(A830GmuWindowGmu,ack,A830_GMU_RSCC_CONTROL_WAKE,A830_GMU_RSCC_CONTROL_WAKE,A830_T_RSCC_US,A830_POLL_STEP_US,
                      true,&value,nullptr);
    if(poll)return fail(A830GmuStepRsccWake,poll==2?A830ReasonReadback:A830ReasonTimeout,A830_GMU_PHYS+ack,value,
                        A830_GMU_RSCC_CONTROL_WAKE,poll==2?kIOReturnIOError:kIOReturnTimeout);
    const uint32_t busy=rsccReg(A8XX_RSCC_SEQ_BUSY_DRV0);
    poll=pollMask(A830GmuWindowRscc,busy,0xffffffff,0,A830_T_RSCC_US,A830_POLL_STEP_US,true,&value,nullptr);
    if(poll)return fail(A830GmuStepRsccWake,poll==2?A830ReasonReadback:A830ReasonTimeout,A830_RSCC_PHYS+busy,value,0,
                        poll==2?kIOReturnIOError:kIOReturnTimeout);
    if(!annibale)(void)wr(A830GmuWindowGmu,request,0);
    else if(!(before&A830_GMU_RSCC_CONTROL_WAKE))(void)wr(A830GmuWindowGmu,request,before);
    barrier();
    rscc_asleep_=false;
    ++report_.rscc_wakes;
    return kIOReturnSuccess;
}

/* GPU RSCC TCS0..9 DRV0 status bit0 (idle): nothing of the GMU's RPMh voting is outstanding. */
IOReturn A830Gmu::waitRsccTcs(uint32_t timeout_us)
{
    IOReturn result=kIOReturnSuccess;
    for(uint32_t tcs=0;tcs<A8XX_RSCC_TCS_COUNT;++tcs) {
        uint32_t value=0;
        const uint32_t offset=rsccReg(A8XX_RSCC_TCS_DRV0_STATUS(tcs));
        const int poll=pollMask(A830GmuWindowRscc,offset,A8XX_RSCC_TCS_IDLE,A8XX_RSCC_TCS_IDLE,timeout_us,A830_POLL_STEP_US,true,
                                &value,nullptr);
        if(poll && result==kIOReturnSuccess) {
            result=poll==2?kIOReturnIOError:kIOReturnTimeout;
            failure_={0,uint16_t(poll==2?A830ReasonReadback:A830ReasonTimeout),A830_RSCC_PHYS+offset,value,A8XX_RSCC_TCS_IDLE};
        }
    }
    if(result!=kIOReturnSuccess)warn(A830GmuWarnTcsBusy);
    return result;
}

IOReturn A830Gmu::haltGbif(uint32_t mask,bool release)
{
    const uint32_t halt=gpuReg(A8XX_GBIF_HALT),ack=gpuReg(A8XX_GBIF_HALT_ACK);
    if(!wr(A830GmuWindowGpu,halt,mask))return kIOReturnIOError;
    barrier();
    uint32_t value=0;
    const int poll=pollMask(A830GmuWindowGpu,ack,mask,mask,A830_T_GBIF_HALT_US,A830_POLL_STEP_US,true,&value,nullptr);
    if(release) {(void)wr(A830GmuWindowGpu,halt,0);barrier();}
    if(poll)failure_={0,uint16_t(poll==2?A830ReasonReadback:A830ReasonTimeout),A830_GPU_PHYS+ack,value,mask};
    return poll==0?kIOReturnSuccess:poll==2?kIOReturnIOError:kIOReturnTimeout;
}

/* --- force-off (ARCHITECTURE 4.7.9) ---------------------------------------------------------------------------------- */

IOReturn A830Gmu::forceOff()
{
    if(!bound_)return kIOReturnNotReady;
    bool access_ok=true;
    auto put=[&](unsigned window,uint32_t offset,uint32_t value) {access_ok=wr(window,offset,value) && access_ok;};
    /* 1. No GMU interrupt while it goes down; GEMNOC kept out of collapse (FALNEXT veto); no keepalive. */
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffff);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffff);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffff);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffff);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_CX_FALNEXT_INTF),A830_GMU_CX_FAL_VETO);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_PWR_COL_KEEPALIVE),0);
    barrier();
    /* 2. GPU RSCC votes complete, 1 ms each, record only. */
    (void)waitRsccTcs(A830_T_RSCC_TCS_FORCE_OFF_US);
    /* 3. AHB fence: dropped-write status cleared, allow mode. */
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS_CLR),A830_AHB_FENCE_STATUS_CLR_VALUE);
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_CTRL),A830_AHB_FENCE_CTRL_ALLOW);
    barrier();
    /* 4. CM3 into reset before anything else: no GMU-initiated access or vote competes with the rest. */
    put(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_CM3_SYSRESET),A830_GMU_CM3_SYSRESET_ASSERT);
    barrier();
    booted_=false;
    /* 5. CX traffic halted at the GBIF arbiter, 100 ms, record only (terminate-mode SMMU: nothing stalls the halt). */
    if(haltGbif(A8XX_GBIF_HALT_ARB,false)!=kIOReturnSuccess)warn(A830GmuWarnGbifHalt);
    /* 6. GX still powered: collapse its GDSC under a temporary host GX vote, then drop the vote. */
    uint32_t gx=0;
    if(rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS),&gx) && !(gx&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF)) {
        const bool voted=hooks_.gx_vote && !hooks_.gx_vote(hooks_.gx_cookie,1);
        if(!voted)warn(A830GmuWarnGxVote);
        uint32_t gdscr=0;
        if(rd(A830GmuWindowGmu,GxGdscr,&gdscr))put(A830GmuWindowGmu,GxGdscr,gdscr|SM8750_GDSC_SW_COLLAPSE);
        else access_ok=false;
        barrier();
        uint32_t cfg=0;
        if(pollMask(A830GmuWindowGmu,GxCfgGdscr,GxPowerDownComplete,GxPowerDownComplete,A830_T_GX_GDSC_OFF_US,
                    A830_T_GX_ON_STEP_US,false,&cfg,nullptr))warn(A830GmuWarnGxStuck);
        if(voted && hooks_.gx_vote(hooks_.gx_cookie,0))warn(A830GmuWarnGxVote);
    }
    /* 7. RSCC sleep (its CM3 reset again is harmless), record only; the GBIF halt released. The power-off of 4.2.3 is the
     * caller's, P1 and later. */
    const A830GmuFailure kept=failure_;
    if(rsccSleep()!=kIOReturnSuccess)warn(A830GmuWarnRsccSleep);
    failure_=kept;
    put(A830GmuWindowGpu,gpuReg(A8XX_GBIF_HALT),0);
    barrier();
    if(rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS),&gx) && !(gx&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF))
        warn(A830GmuWarnGxStillOn);
    oob_refs_[0]=oob_refs_[1]=0;
    __atomic_store_n(&info_pending_,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&ao_pending_,0u,__ATOMIC_RELEASE);
    ++report_.force_offs;
    return access_ok?kIOReturnSuccess:kIOReturnIOError;
}

/* --- slumber, GMU side (ARCHITECTURE 4.17 P1) ---------------------------------------------------------------------- */

IOReturn A830Gmu::slumber(const A830GmuSlumberOps &ops)
{
    if(!bound_ || !booted_)return kIOReturnNotReady;
    auto step=[](unsigned n) {return uint16_t(A830GmuStepSlumber+n);};
    /* S2: GX on through the OOB; an interrupt the GPU still owes ends the attempt with nothing changed. */
    IOReturn status=oobSet(nullptr);
    if(status!=kIOReturnSuccess) {failure_.step=step(2);return status;}
    if(ops.gpu_irq_pending && ops.gpu_irq_pending(ops.cookie)) {
        oobClear();
        ++report_.slumber_aborts;
        return kIOReturnBusy;
    }
    if(ops.gpu_irq_off)ops.gpu_irq_off(ops.cookie);
    oobClear();
    /* S3: the GMU reached its idle level (IFPC: and GX is off). */
    {
        const uint64_t begin=os_.now_us(os_.cookie);
        uint32_t state=0,gx=0;
        for(;;) {
            const bool read=rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_RPMH_POWER_STATE),&state) &&
                rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GFX_PWR_CLK_STATUS),&gx);
            if(!read)return fail(step(3),A830ReasonReadback,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_RPMH_POWER_STATE),state,
                                 config_.idle_level,kIOReturnIOError);
            const bool gx_on=!(gx&A8XX_GMUCX_GFX_PWR_CLK_STATUS_GX_OFF);
            if(state==config_.idle_level && !(config_.idle_level==A830_GMU_RPMH_POWER_STATE_IFPC && gx_on))break;
            if(os_.now_us(os_.cookie)-begin>=A830_T_LOWEST_IDLE_US)
                return fail(step(3),A830ReasonTimeout,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_RPMH_POWER_STATE),state,config_.idle_level,
                            kIOReturnTimeout);
            os_.sleep_us(os_.cookie,A830_POLL_STEP_US);
        }
    }
    /* S4: the GMU's RPMh votes complete. */
    status=waitRsccTcs(A830_T_RSCC_TCS_SLUMBER_US);
    if(status!=kIOReturnSuccess) {failure_.step=step(4);return status;}
    /* S5: power counters off (the GMU is then not busy), PREPARE_SLUMBER, fence in allow mode, FALNEXT veto. The two
     * writes after the message happen whatever its answer (KGSL gen8_gmu_notify_slumber). */
    bool access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_POWER_COUNTER_ENABLE),0);
    const int prepared=ops.prepare_slumber?ops.prepare_slumber(ops.cookie):0;
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_CTRL),A830_AHB_FENCE_CTRL_ALLOW) && access_ok;
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_CX_FALNEXT_INTF),A830_GMU_CX_FAL_VETO) && access_ok;
    barrier();
    if(prepared)return fail(step(5),A830ReasonHfiStatus,A830_HFI_H2F_PREPARE_SLUMBER,uint32_t(prepared),0,kIOReturnIOError);
    if(!access_ok)return fail(step(5),A830ReasonReadback,A830_GMU_PHYS+gmuReg(A8XX_GMUAO_AHB_FENCE_CTRL),0,0,kIOReturnIOError);
    /* S6: the GMU stopped being busy (KGSL gen8 bit30; the Linux bit23 is not used, ARCHITECTURE 9.2 #4). */
    uint32_t value=0;
    const uint32_t busy=gmuReg(A8XX_GMUAO_GPU_CX_BUSY_STATUS);
    const int poll=pollMask(A830GmuWindowGmu,busy,config_.cx_busy_mask,0,A830_T_CX_IDLE_US,A830_POLL_STEP_US,true,&value,nullptr);
    if(poll)return fail(step(6),poll==2?A830ReasonReadback:A830ReasonTimeout,A830_GMU_PHYS+busy,value,0,
                        poll==2?kIOReturnIOError:kIOReturnTimeout);
    /* S7: RSCC sleep (saves GENERAL_9). */
    status=rsccSleep();
    if(status!=kIOReturnSuccess) {failure_.step=step(7);return status;}
    /* S8: GBIF client halt, then arbiter halt, then released (KGSL gen8_halt_gbif). */
    status=haltGbif(A8XX_GBIF_HALT_CLIENT,false);
    if(status==kIOReturnSuccess)status=haltGbif(A8XX_GBIF_HALT_ARB,false);
    (void)wr(A830GmuWindowGpu,gpuReg(A8XX_GBIF_HALT),0);
    barrier();
    if(status!=kIOReturnSuccess) {failure_.step=step(8);return status;}
    /* S9: GMU interrupts masked and cleared. */
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffff);
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffff) && access_ok;
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),0xffffffff) && access_ok;
    access_ok=wr(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),0xffffffff) && access_ok;
    barrier();
    if(!access_ok)return fail(step(9),A830ReasonReadback,A830_GMU_PHYS+gmuReg(A8XX_GMUCX_GMU2HOST_INTR_MASK),0xffffffff,0,
                              kIOReturnIOError);
    booted_=false;
    ++report_.slumbers;
    return kIOReturnSuccess;
}

/* --- IFPC helpers (ARCHITECTURE 4.17 P2, 4.13.2) ------------------------------------------------------------------- */

IOReturn A830Gmu::keepalive(bool on)
{
    if(!bound_)return kIOReturnNotReady;
    if(!wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_PWR_COL_KEEPALIVE),on?1u:0u))return kIOReturnIOError;
    barrier();
    return kIOReturnSuccess;
}

IOReturn A830Gmu::waitFenceAllow(uint32_t polls)
{
    if(!bound_)return kIOReturnNotReady;
    const uint32_t control=gmuReg(A8XX_GMUAO_AHB_FENCE_CTRL);
    for(uint32_t i=0;i<polls;++i) {
        uint32_t value=0;
        if(!rd(A830GmuWindowGmu,control,&value))return kIOReturnIOError;
        if(value==A830_AHB_FENCE_CTRL_ALLOW)return kIOReturnSuccess;
        os_.delay_us(os_.cookie,1);
    }
    return kIOReturnTimeout;
}

IOReturn A830Gmu::fencedWrite(unsigned window,uint32_t offset,uint32_t value,uint32_t *retries)
{
    if(retries)*retries=0;
    if(!bound_)return kIOReturnNotReady;
    const uint32_t status=gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS);
    for(uint32_t attempt=0;attempt<FenceRetries;++attempt) {
        if(!wr(window,offset,value))return kIOReturnIOError;
        barrier();
        uint32_t dropped=0;
        if(!rd(A830GmuWindowGmu,status,&dropped))return kIOReturnIOError;
        if(!(dropped&A830_AHB_FENCE_STATUS_WRITEDROPPED0)) {
            if(retries)*retries=attempt;
            report_.fenced_retries+=attempt;
            return kIOReturnSuccess;
        }
        os_.delay_us(os_.cookie,A830_WPTR_FENCE_RETRY_US);
    }
    if(retries)*retries=FenceRetries;
    report_.fenced_retries+=FenceRetries;
    return kIOReturnTimeout;
}

/* --- interrupts (ARCHITECTURE 4.12) --------------------------------------------------------------------------------- */

void A830Gmu::kick()
{
    if(irq_ && irq_->work_kick && work_)irq_->work_kick(work_);
}

IOReturn A830Gmu::attachInterrupts(const A830GmuIrqOps &ops)
{
    if(!bound_)return kIOReturnNotReady;
    if(irq_ || report_.polled)return kIOReturnBusy;
    /* annibale installs no GMU interrupt handler: B2 left both sources masked and their fatal bits are polled
     * (kernel-sequences section 14 row 18, K/adreno8_gmu.c:576-580). Off by choice, so adreno830_status.irq_off (whose
     * GMU bits mean "attach failed or storm") stays clear. */
    if(adreno830_annibale()) {
        report_.polled=A830GmuIrqHfi|A830GmuIrqAo;
        __atomic_fetch_or(&irq_off_,report_.polled,__ATOMIC_RELAXED);
        return kIOReturnSuccess;
    }
    irq_=&ops;
    work_=ops.work_allocate?ops.work_allocate(this):nullptr;
    const struct {uint32_t intid;void (*handler)(void *);uint32_t bit,status_bit;} lines[]={
        {config_.hfi_irq,hfiIrq,A830GmuIrqHfi,A830StatusIrqOffHfi},{config_.gmu_irq,aoIrq,A830GmuIrqAo,A830StatusIrqOffGmu}};
    for(unsigned i=0;i<2;++i) {
        uint32_t readback=0;uint64_t router=0;
        const IOReturn status=ops.attach && lines[i].intid?ops.attach(lines[i].intid,false,lines[i].handler,this,&readback,&router)
                                                          :kIOReturnUnsupported;
        report_.irq_readback[i]=readback;
        bool off=status!=kIOReturnSuccess || !work_;
        if(status==kIOReturnSuccess) {
            irq_wired_|=lines[i].bit;
            if((readback&StatusLevelMask)!=StatusLevelReadback) {warn(A830GmuWarnIrqReadback);off=true;}
        }
        if(off) {
            __atomic_fetch_or(&irq_off_,lines[i].bit,__ATOMIC_RELAXED);
            __atomic_fetch_or(&adreno830_status.irq_off,lines[i].status_bit,__ATOMIC_RELAXED);
        }
    }
    return kIOReturnSuccess;
}

IOReturn A830Gmu::reenableInterrupts()
{
    if(!bound_)return kIOReturnNotReady;
    if(!irq_)return kIOReturnSuccess;
    const struct {uint32_t intid,bit,status_bit;} lines[]={
        {config_.hfi_irq,A830GmuIrqHfi,A830StatusIrqOffHfi},{config_.gmu_irq,A830GmuIrqAo,A830StatusIrqOffGmu}};
    IOReturn result=kIOReturnSuccess;
    for(const auto &line:lines) {
        if(!(__atomic_load_n(&storm_off_,__ATOMIC_RELAXED)&line.bit))continue;
        if(!irq_->enable || irq_->enable(line.intid)!=kIOReturnSuccess) {result=kIOReturnUnsupported;continue;}
        __atomic_fetch_and(&storm_off_,~line.bit,__ATOMIC_RELAXED);
        if(line.bit==A830GmuIrqHfi)hfi_idle_=0;else ao_idle_=0;
        __atomic_fetch_and(&irq_off_,~line.bit,__ATOMIC_RELAXED);
        __atomic_fetch_and(&adreno830_status.irq_off,~line.status_bit,__ATOMIC_RELAXED);
    }
    return result;
}

/* Storm guard (ARCHITECTURE 4.12.1): the source masked on the device side, the SPI disabled when PaganiArmCpu can, irq_off
 * set (reason 23); the fatal bits are polled from then on. Interrupt context. */
void A830Gmu::stormOff(unsigned which)
{
    const bool hfi=which==A830GmuIrqHfi;
    (void)wr(A830GmuWindowGmu,gmuReg(hfi?A8XX_GMUCX_GMU2HOST_INTR_MASK:A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),0xffffffff);
    barrier();
    if(irq_ && irq_->disable)(void)irq_->disable(hfi?config_.hfi_irq:config_.gmu_irq);
    __atomic_fetch_or(&storm_off_,which,__ATOMIC_RELAXED);
    __atomic_fetch_or(&irq_off_,which,__ATOMIC_RELAXED);
    __atomic_fetch_or(&adreno830_status.irq_off,hfi?A830StatusIrqOffHfi:A830StatusIrqOffGmu,__ATOMIC_RELAXED);
    __atomic_fetch_or(&events_,uint32_t(EventStorm),__ATOMIC_RELEASE);
    warn(A830GmuWarnStorm);
    __atomic_fetch_add(&report_.storms,1u,__ATOMIC_RELAXED);
    kick();
}

void A830Gmu::hfiIrq(void *context)
{
    A830Gmu *gmu=static_cast<A830Gmu *>(context);
    if(!gmu || !gmu->bound_)return;
    const uint32_t info_reg=gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO);
    uint32_t info=0;
    if(gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,info_reg,&info))info=0;
    /* Every opened bit is cleared whether or not anything handles it (KGSL clears HFI_IRQ_MASK unconditionally): bit1
     * SIDEMSGQ has no handler and would otherwise be a level storm. The OOB acknowledgement bits are not touched. */
    (void)gmu->io_.write32(gmu->io_.cookie,A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),A830_GMU2HOST_IRQ_CLEAR);
    gmu->barrier();
    const uint32_t opened=info&A830_GMU2HOST_IRQ_CLEAR;
    if(opened)__atomic_fetch_or(&gmu->info_pending_,opened,__ATOMIC_RELEASE);
    if(opened&A830_GMU2HOST_CM3_FAULT)__atomic_store_n(&gmu->fatal_,1u,__ATOMIC_RELEASE);
    __atomic_fetch_add(&gmu->report_.hfi_irqs,1u,__ATOMIC_RELAXED);
    uint32_t after=0;
    if(gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,info_reg,&after))after=0;
    const bool idle=!opened || (after&A830_GMU2HOST_IRQ_CLEAR)==opened;
    if(!idle)gmu->hfi_idle_=0;
    else if(++gmu->hfi_idle_>=A830GmuStormLimit && !(__atomic_load_n(&gmu->storm_off_,__ATOMIC_RELAXED)&A830GmuIrqHfi)) {
        gmu->stormOff(A830GmuIrqHfi);
        return;
    }
    if(opened)gmu->kick();
}

void A830Gmu::aoIrq(void *context)
{
    A830Gmu *gmu=static_cast<A830Gmu *>(context);
    if(!gmu || !gmu->bound_)return;
    const uint32_t status_reg=gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS);
    uint32_t status=0;
    if(gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,status_reg,&status))status=0;
    (void)gmu->io_.write32(gmu->io_.cookie,A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),status);
    if(status&A830_GMU_AO_WDOG_BITE) {
        /* The bite is masked at once so it cannot storm before the bottom half runs (KGSL gen8_gmu_handle_watchdog). */
        uint32_t mask=0;
        if(!gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),&mask))
            (void)gmu->io_.write32(gmu->io_.cookie,A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_MASK),
                                   mask|A830_GMU_AO_WDOG_BITE);
        __atomic_store_n(&gmu->fatal_,1u,__ATOMIC_RELEASE);
    }
    if(status&A830_GMU_AO_FENCE_ERR) {
        uint32_t fence=0;
        if(!gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS),&fence))
            __atomic_store_n(&gmu->report_.fence_status,fence,__ATOMIC_RELAXED);
    }
    gmu->barrier();
    const uint32_t handled=status&(A830_GMU_AO_WDOG_BITE|A830_GMU_AO_FENCE_ERR|A830_GMU_AO_HOST_AHB_BUS_ERR);
    if(status)__atomic_store_n(&adreno830_status.sticky[3],status,__ATOMIC_RELAXED);
    if(handled)__atomic_fetch_or(&gmu->ao_pending_,handled,__ATOMIC_RELEASE);
    __atomic_fetch_add(&gmu->report_.ao_irqs,1u,__ATOMIC_RELAXED);
    uint32_t after=0;
    if(gmu->io_.read32(gmu->io_.cookie,A830GmuWindowGmu,status_reg,&after))after=0;
    const bool idle=!status || (after&status)==status;
    if(!idle)gmu->ao_idle_=0;
    else if(++gmu->ao_idle_>=A830GmuStormLimit && !(__atomic_load_n(&gmu->storm_off_,__ATOMIC_RELAXED)&A830GmuIrqAo)) {
        gmu->stormOff(A830GmuIrqAo);
        return;
    }
    if(handled)gmu->kick();
}

void A830Gmu::gmuWork()
{
    if(!bound_)return;
    __atomic_fetch_add(&report_.works,1u,__ATOMIC_RELAXED);
    const uint32_t info=__atomic_exchange_n(&info_pending_,0u,__ATOMIC_ACQ_REL);
    const uint32_t ao=__atomic_exchange_n(&ao_pending_,0u,__ATOMIC_ACQ_REL);
    const uint32_t events=__atomic_exchange_n(&events_,0u,__ATOMIC_ACQ_REL);
    bool publish=events&EventStorm;
    uint16_t reason=0;
    /* The debug queue is drained by the thread holding the HFI mutex (A830Hfi::drainDebug takes it itself). */
    if(info && hooks_.drain_debug && hooks_.drain_debug(hooks_.cookie))reason=A830ReasonF2hError;
    if(hooks_.lock)hooks_.lock(hooks_.cookie);
    recordInterrupts(info,ao);
    if((info&A830_GMU2HOST_CM3_FAULT) || (ao&A830_GMU_AO_WDOG_BITE))reason=A830ReasonGmuFault;
    if(reason) {
        __atomic_fetch_add(&adreno830_status.gmu_faults,1u,__ATOMIC_RELAXED);
        ++report_.faults;
        if(hooks_.fault)hooks_.fault(hooks_.cookie,reason,info,ao);
        publish=true;
    }
    if(hooks_.unlock)hooks_.unlock(hooks_.cookie);
    if(publish)adreno830_status_publish();
}

IOReturn A830Gmu::pollFatal()
{
    if(!bound_ || !booted_)return kIOReturnNotReady;
    const uint32_t off=__atomic_load_n(&irq_off_,__ATOMIC_RELAXED);
    if(!off && irq_)return kIOReturnSuccess;
    ++report_.polls;
    if((off&A830GmuIrqHfi) || !irq_) {
        uint32_t info=0;
        if(rd(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_INFO),&info) && (info&A830_GMU2HOST_IRQ_CLEAR)) {
            (void)wr(A830GmuWindowGmu,gmuReg(A8XX_GMUCX_GMU2HOST_INTR_CLR),info&A830_GMU2HOST_IRQ_CLEAR);
            __atomic_fetch_or(&info_pending_,info&A830_GMU2HOST_IRQ_CLEAR,__ATOMIC_RELEASE);
            if(info&A830_GMU2HOST_CM3_FAULT)__atomic_store_n(&fatal_,1u,__ATOMIC_RELEASE);
        }
    }
    if((off&A830GmuIrqAo) || !irq_) {
        uint32_t status=0;
        if(rd(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_STATUS),&status) && status) {
            (void)wr(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AO_HOST_INTERRUPT_CLR),status);
            if(status&A830_GMU_AO_FENCE_ERR)(void)rd(A830GmuWindowGmu,gmuReg(A8XX_GMUAO_AHB_FENCE_STATUS),&report_.fence_status);
            if(status&A830_GMU_AO_WDOG_BITE)__atomic_store_n(&fatal_,1u,__ATOMIC_RELEASE);
            __atomic_store_n(&adreno830_status.sticky[3],status,__ATOMIC_RELAXED);
            __atomic_fetch_or(&ao_pending_,status&(A830_GMU_AO_WDOG_BITE|A830_GMU_AO_FENCE_ERR|A830_GMU_AO_HOST_AHB_BUS_ERR),
                              __ATOMIC_RELEASE);
        }
    }
    barrier();
    gmuWork();
    return kIOReturnSuccess;
}

/* --- kernel services -------------------------------------------------------------------------------------------------- */

#ifndef ADRENO830_HOST_TEST
#include "NativeAdrenoKernelTime.h"
#include <kern/thread_call.h>

extern "C" {
/* PaganiArmCpu (PD drivers/PaganiArmCpu/ArmCpu.cpp) exports armcpu_spi_attach/disable/enable (ARCHITECTURE 4.12.1). The
 * weak defaults the statically linked kernel needs (it refuses undefined weak references) are A830Irq.cpp's, one set for the
 * whole archive; with them the GMU interrupts stay off and their fatal bits are polled. */
IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
IOReturn armcpu_spi_disable(uint32_t);
IOReturn armcpu_spi_enable(uint32_t);
}

namespace {
uint64_t kernelNow(void *) {return NativeAdrenoKernelTime::monotonic_us();}
void kernelSleep(void *,uint32_t us) {NativeAdrenoKernelTime::sleep_us(us);}
void kernelDelay(void *,uint32_t us) {NativeAdrenoKernelTime::spin_us(us);}
uint64_t kernelCounter(void *) {return NativeAdrenoKernelTime::counter();}
uintptr_t kernelIrqOff(void *) {return NativeAdrenoKernelTime::interruptsOff();}
void kernelIrqRestore(void *,uintptr_t state) {NativeAdrenoKernelTime::interruptsRestore(state);}
void kernelWork(thread_call_param_t gmu,thread_call_param_t) {static_cast<A830Gmu *>(gmu)->gmuWork();}
void *kernelWorkAllocate(A830Gmu *gmu)
{
    return thread_call_allocate_with_options(kernelWork,gmu,THREAD_CALL_PRIORITY_KERNEL_HIGH,THREAD_CALL_OPTIONS_ONCE);
}
void kernelWorkKick(void *call) {(void)thread_call_enter(static_cast<thread_call_t>(call));}
}

const A830GmuOs *A830GmuKernelOs()
{
    static const A830GmuOs os={nullptr,kernelNow,kernelSleep,kernelDelay,kernelCounter,kernelIrqOff,kernelIrqRestore};
    return &os;
}

const A830GmuIrqOps *A830GmuKernelIrqOps()
{
    static const A830GmuIrqOps ops={armcpu_spi_attach,armcpu_spi_disable,armcpu_spi_enable,kernelWorkAllocate,kernelWorkKick};
    return &ops;
}
#endif
