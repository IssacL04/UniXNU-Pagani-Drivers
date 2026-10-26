// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalHidDeviceABI.h"
#include "Mapping.h"
#include "Buttons.h"
#include <PaganiPeripherals.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
extern "C" {
#include "S3910.h"
#include <GeniSpi.h>
}

#include "Reports.h"
#include "../BootProgress/PeripheralStatus.h"

class PaganiInput : public IOHIDDevice {
    OSDeclareDefaultStructors(PaganiInput)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool touch=false,running=false,timerAdded=false;
    PeripheralWindow core,cnfg,obs,gcc,se,wrapper,pins[6];
    geni_spi spi={};
    s3910 controller={};
    s3910_frame previous={};
    pagani_button_filter filter={};
    int pon=-1,gpio=-1;
    uint8_t lastButtons=0;
    uint32_t savedPins[6]={};
    uint32_t savedReset=0;
    unsigned pinsConfigured=0;
    const char *phase="allocate";
    uint32_t sample=0;
    uint64_t reports=0;
    int touchError=0;
    char touchPhase[24]={};
    void observe(const char *stage,int result=0) {
        phase=stage;
        boot_progress_peripheral(touch?PAGANI_TOUCH:PAGANI_KEYS,phase,result,sample,reports);
    }
    void observeTouch(int result) {
        snprintf(touchPhase,sizeof touchPhase,"T%u %02x %s",controller.version,controller.command,controller.stage);
        sample=controller.detail;observe(touchPhase,result);
    }
    int transferTouch(const uint8_t *tx,uint8_t *rx,size_t size) {
        spi.snap_valid=0;
        int rc=geni_spi_transfer(&spi,tx,rx,size,0);
        // Preserve the driver's pre-cancel snapshot; live registers now describe recovery.
        if (rc && spi.snap_valid) boot_progress_touch_spi(spi.info.fw_revision,spi.info.if_disable,
            spi.snap_status,spi.snap_irq,spi.snap_txfs,spi.snap_rxfs);
        return rc;
    }
    static int readTouch(void *ctx,uint8_t *data,size_t size) {
        uint8_t dummy[S3910_PAYLOAD_MAX+7];
        if (!data || !size) return -22;
        if (size>sizeof dummy) return -90;
        // Synaptics reads clock 0xff on MOSI, as in the stock HBP SPI transport.
        memset(dummy,0xff,size);
        return static_cast<PaganiInput *>(ctx)->transferTouch(dummy,data,size);
    }
    static int writeTouch(void *ctx,const uint8_t *data,size_t size) {
        return static_cast<PaganiInput *>(ctx)->transferTouch(data,nullptr,size);
    }
    static void sleepTouch(void *,unsigned ms) {IOSleep(ms);}
    int setupButtons() {
        observe("SPMI map");
        if (!core.map(PAGANI_SPMI_CORE,0x3000) || !cnfg.map(PAGANI_SPMI_CNFG,0x4000) || !obs.map(PAGANI_SPMI_OBS,0x80000)) return -12;
        auto c=core.io(),f=cnfg.io();
        observe("SPMI channels");
        pon=spmi_find(&c,&f,PAGANI_PON_PPID,PAGANI_SPMI_EE);
        gpio=spmi_find(&c,&f,PAGANI_GPIO_PPID,PAGANI_SPMI_EE);
        sample=((uint32_t)pon<<16)|((uint32_t)gpio&0xffff);
        if (pon<0 || gpio<0) return -19;
        auto o=obs.io();uint8_t subtype=0,type=0,mode=0,enable=0;
        observe("PON subtype");
        int rc=spmi_read8(&o,PAGANI_SPMI_EE,pon,5,&subtype);sample=subtype;
        if (rc || subtype!=9) return rc?rc:-19;
        observe("GPIO type");rc=spmi_read8(&o,PAGANI_SPMI_EE,gpio,4,&type);sample=type;
        if (rc || type!=0x10) return rc?rc:-19;
        observe("GPIO subtype");rc=spmi_read8(&o,PAGANI_SPMI_EE,gpio,5,&subtype);sample=subtype;
        if (rc || subtype<0x10 || subtype>0x13) return rc?rc:-19;
        observe("GPIO mode");rc=spmi_read8(&o,PAGANI_SPMI_EE,gpio,0x40,&mode);sample=mode;
        if (rc || (mode&3)) return rc?rc:-19;
        observe("GPIO enable");rc=spmi_read8(&o,PAGANI_SPMI_EE,gpio,0x46,&enable);sample=enable;
        if (rc || !(enable&0x80)) return rc?rc:-19;
        return 0;
    }
    int setupTouch() {
        observe("GCC map");
        if (!gcc.map(PAGANI_GCC_BASE,0x60000)) return -12;
        auto g=gcc.io();unsigned hz=0,select=0;
        observe("SPI clock");
        int rc=sm8750_touch_clock(&g,&hz,&select);if (rc) return rc;
        sample=hz;observe("SPI map");
        if (!se.map(PAGANI_TOUCH_SE,0x4000) || !wrapper.map(PAGANI_TOUCH_WRAPPER,0x1000)) return -12;
        spi.se={&se,PeripheralWindow::read,PeripheralWindow::write,PeripheralWindow::delay};
        spi.wrapper={&wrapper,PeripheralWindow::read,PeripheralWindow::write,PeripheralWindow::delay};
        // shortcut: adopt boot firmware rails and SPI microcode; cold power-up needs RPMh rail votes.
        observe("GENI firmware");rc=geni_spi_read_info(&spi);if (rc) return rc;
        sample=spi.info.geni_status;
        if (spi.info.geni_status&GENI_STATUS_M_ACTIVE) return -16;
        spi.poll_only=1;spi.poll_limit=20000;spi.allow_fifo_disabled=1;
        observe("SPI init");rc=geni_spi_init(&spi);if (rc) return rc;
        observe("SPI mode");rc=geni_spi_setup(&spi,0,0);if (rc) return rc;
        observe("SPI rate");rc=geni_spi_set_clock(&spi,PAGANI_TOUCH_HZ,hz,select);if (rc) return rc;
        const unsigned gpios[]=PAGANI_TOUCH_PINS;
        const unsigned drives[]=PAGANI_TOUCH_DRIVES;
        for (unsigned i=0;i<6;++i) {
            sample=gpios[i];observe("touch GPIO");
            if (!pins[i].map(PAGANI_TLMM_GPIO(gpios[i]),0x1000)) return -12;
            savedPins[i]=PeripheralWindow::read(&pins[i],0);
            uint32_t cfg=(savedPins[i]&~0x3ffu)|(drives[i]<<6)|(i<4?4:0);
            if (i==4) {
                savedReset=PeripheralWindow::read(&pins[i],4)&2;
                PeripheralWindow::write(&pins[i],4,2); // Set RESET high before enabling output.
                cfg|=1u<<9;
            }
            PeripheralWindow::write(&pins[i],0,cfg);++pinsConfigured;
        }
        PeripheralWindow::write(&pins[4],4,0);IOSleep(PAGANI_TOUCH_RESET_MS);
        PeripheralWindow::write(&pins[4],4,2);IOSleep(PAGANI_TOUCH_STARTUP_MS);
        controller.bus={this,readTouch,writeTouch,sleepTouch};
        observe("TouchComm start");rc=s3910_start(&controller);
        observeTouch(rc);
        return rc;
    }
    bool send(const void *bytes,size_t size) {
        auto buffer=IOBufferMemoryDescriptor::withBytes(bytes,size,kIODirectionNone);
        IOReturn rc=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        if (!rc) ++reports;
        observe("HID report",rc);
        OSSafeReleaseNULL(buffer);return rc==kIOReturnSuccess;
    }
    void releaseContacts() {
        bool active=false;for (auto &c:previous.contact) active|=c.active!=0;
        if (!active) return;
        s3910_frame empty={};uint8_t report[62];s3910_hid_report(&empty,&previous,report);
        if (send(report,sizeof report)) previous=empty;
    }
    static void tick(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<PaganiInput *>(owner);
        if (!__atomic_load_n(&self->running,__ATOMIC_ACQUIRE)) return;
        if (self->touch) {
            if (self->controller.ready) self->sample=PeripheralWindow::read(&self->pins[5],4)&1;
            if (self->controller.ready && !self->sample) {
                s3910_frame frame={};int rc=s3910_poll(&self->controller,&frame);
                if (rc==1) {
                    self->sample=0;for (unsigned i=0;i<S3910_CONTACTS;++i) if (frame.contact[i].active) self->sample|=1u<<i;
                    uint8_t report[62];s3910_hid_report(&frame,&self->previous,report);
                    if (self->send(report,sizeof report)) self->previous=frame;
                } else if (rc<0) {
                    self->controller.ready=0;self->touchError=rc;self->setProperty("InputError",(uint32_t)rc,32);
                } else {
                    self->sample=self->controller.code;self->observe("TouchComm idle");
                }
            }
            else if (self->controller.ready && !self->reports) self->observe("ATTN wait");
            if (!self->controller.ready) {self->releaseContacts();self->observeTouch(self->touchError);}
        } else {
            uint8_t p=0,g=1;auto io=self->obs.io();
            int rc=spmi_read8(&io,PAGANI_SPMI_EE,self->pon,0x10,&p);
            if (!rc) rc=spmi_read8(&io,PAGANI_SPMI_EE,self->gpio,0x10,&g);
            uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
            uint8_t mask=pagani_buttons_filter(&self->filter,!rc,pagani_buttons(p,g),ns/1000000);
            self->sample=((uint32_t)p<<8)|g;
            if (mask!=self->lastButtons) {uint8_t report[]={2,mask};if (self->send(report,2)) self->lastButtons=mask;}
            else self->observe("PMIC sample",rc);
        }
        // shortcut: poll the ATTN line and PMIC levels until GIC detach/lifetime support is available.
        self->timer->setTimeoutMS(self->touch?8:16);
    }
    void cleanup() {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (timerAdded) loop->removeEventSource(timer);}
        timerAdded=false;OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);
        for (unsigned i=0;i<pinsConfigured;++i) {
            if (i==4) PeripheralWindow::write(&pins[i],4,savedReset);
            PeripheralWindow::write(&pins[i],0,savedPins[i]);
        }
        pinsConfigured=0;for (auto &pin:pins) pin.release();
        core.release();cnfg.release();obs.release();se.release();wrapper.release();gcc.release();
    }
public:
    void selectTouch() {touch=true;}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    OSString *newTransportString() const override {return OSString::withCString(touch?"SPI":"SPMI");}
    OSString *newProductString() const override {return OSString::withCString(touch?"Pagani S3910":"Pagani buttons");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if (!out) return kIOReturnBadArgument;
        *out=IOBufferMemoryDescriptor::withBytes(touch?touchDescriptor:buttonsDescriptor,
            touch?sizeof touchDescriptor:sizeof buttonsDescriptor,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        observe("work loop");
        loop=IOWorkLoop::workLoop();if (!loop) {observe(phase,kIOReturnNoMemory);return false;}
        setProperty("HIDDefaultBehavior",true);setProperty("Built-In",true);
        if (touch) {setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        int rc=touch?setupTouch():setupButtons();
        if (rc) {observe(phase,rc);IOLog("Pagani %s %s failed: %d\n",touch?"touch":"buttons",phase,rc);cleanup();return false;}
        observe("HID start");
        if (!IOHIDDevice::start(provider)) {observe(phase,kIOReturnError);cleanup();return false;}
        timer=IOTimerEventSource::timerEventSource(this,tick);
        timerAdded=timer && loop->addEventSource(timer)==kIOReturnSuccess;
        if (!timerAdded) {observe("timer",kIOReturnNoResources);IOHIDDevice::stop(provider);cleanup();return false;}
        observe("ready");
        __atomic_store_n(&running,true,__ATOMIC_RELEASE);timer->setTimeoutMS(1);return true;
    }
    void stop(IOService *provider) override {
        __atomic_store_n(&running,false,__ATOMIC_RELEASE);
        if (timer) {timer->cancelTimeout();if (timerAdded) {loop->removeEventSource(timer);timerAdded=false;}}
        if (touch) releaseContacts();else {uint8_t report[]={2,0};send(report,2);}
        IOHIDDevice::stop(provider);cleanup();
    }
    void free() override {cleanup();IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(PaganiInput,IOHIDDevice)
extern "C" IOService *pagani_input_allocate(bool touch) {
    auto device=new PaganiInput;if (device && touch) device->selectTouch();return device;
}
