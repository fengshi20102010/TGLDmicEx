// Phase 3: replay the Linux SOF IPC3 sequence for PCM 6 "DMIC" (pipeline 10) and capture 1 s
// of 4-channel 48 kHz S32_LE audio from the internal mic array into an OSData snapshot.
// Host side mirrors Linux hda_dsp_stream_hw_params() + hda_dsp_pcm_trigger(); all polled.
#include "TGLDmic.hpp"
#include "Regs.hpp"
#include "IpcReplay.hpp"
#include "IpcReplay2ch.hpp"
#include "Nhlt.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <kern/clock.h>

#define LOG(fmt, ...) logLine(fmt, ##__VA_ARGS__)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

namespace {
constexpr uint32_t SD_CTL_INT_MASK = 0x1C, SD_TAG_MASK = 0xFu << 20;
constexpr uint32_t VS_DPIB_BASE = 0x1084, VS_DPIB_STRIDE = 0x20;
constexpr uint32_t SPB_SPBFCCTL = 0x04;
constexpr uint32_t RING_BYTES = 0x10000, PERIOD_BYTES = 0x4000, PAGES = RING_BYTES / 4096;
// HDA SD_FMT: BASE 48 kHz, BITS = 100 (32-bit container), CHAN = channels - 1
constexpr uint16_t FMT_48K_32BIT = 0x0040;
constexpr uint32_t CAPTURE_SECONDS = 8, ENV_STEP_FRAMES = 4800;   // envelope: 100 ms steps
constexpr uint32_t SNAPSHOT_FRAMES = 48000;                       // 1 s

// Captured pipelines (linux/extract-ipc.py). Both use the same SOF topology (sof-hda-generic*.tplg,
// pipeline 10); the 2 ch one differs in DAI_CONFIG (1 PDM controller), buffer sizes, the volume
// component's channel count and the PCM channel count.
const IpcPipeline kPipe4ch = { kIpc_setup, COUNT(kIpc_setup), kIpc_start, COUNT(kIpc_start), kIpc_stop,
    COUNT(kIpc_stop), kIpc_teardown, COUNT(kIpc_teardown), 4, "HP EliteBook 840 G8" };
const IpcPipeline kPipe2ch = { kIpc2ch_setup, COUNT(kIpc2ch_setup), kIpc2ch_start, COUNT(kIpc2ch_start),
    kIpc2ch_stop, COUNT(kIpc2ch_stop), kIpc2ch_teardown, COUNT(kIpc2ch_teardown), 2,
    "Lenovo XiaoXin Pro 13 2020" };
}

// tgldmicch=<2|4> wins; otherwise the ACPI NHLT table decides (1-2 mics -> 2 ch pipeline, 3+ -> 4 ch);
// without a usable NHLT keep the original 4 ch pipeline.
void TGLDmic::selectDmicPipeline() {
    uint32_t ch = 0;
    // The NHLT is always read and logged, even when tgldmicch= overrides it, so one boot shows whether
    // the detection works on this machine.
    // IOACPIPlatformExpert::getACPITableData is protected; the public path is the controller's own
    // ACPI node (IOPCIDevice "acpi-device", e.g. _SB.PCI0.HDAS), which forwards to the platform.
    auto pe = pci ? OSDynamicCast(IOACPIPlatformDevice, pci->getProperty("acpi-device")) : nullptr;
    const OSData *t = pe ? pe->getACPITableData("NHLT", 0) : nullptr;   // owned by the platform expert
    uint32_t nhltCh = 0;
    nhltBytes = nhltMics = nhltArrayType = 0;
    nhltStatus = pe ? "no NHLT table" : "controller has no acpi-device";
    if (t && t->getLength()) {
        auto info = Nhlt::dmicInfo(static_cast<const uint8_t *>(t->getBytesNoCopy()), t->getLength());
        nhltBytes = t->getLength(); nhltMics = info.mics; nhltArrayType = info.arrayType;
        nhltStatus = info.mics ? "ok" : "table present, no PDM capture endpoint";
        LOG("dmic: NHLT %u bytes: %u PDM mic(s), array type 0x%02x%s", t->getLength(), info.mics,
            info.arrayType, info.fromFormats ? " (from format channels)" : "");
        if (info.mics) {
            nhltCh = info.mics <= 2 ? 2 : 4;
            if (info.mics != 2 && info.mics != 4)
                LOG("dmic: no captured pipeline for %u mics - using the %u ch one", info.mics, nhltCh);
        }
    } else {
        LOG("dmic: no NHLT table (%s)", pe ? "not present" : "controller has no acpi-device");
    }
    int arg = 0;
    if (PE_parse_boot_argn("tgldmicch", &arg, sizeof(arg))) {
        if (arg == 2 || arg == 4) {
            ch = (uint32_t)arg; dmicSource = "boot-arg tgldmicch";
            if (nhltCh && nhltCh != ch) LOG("dmic: WARNING tgldmicch=%d disagrees with the NHLT (%u ch)", arg, nhltCh);
        } else {
            LOG("dmic: ignoring tgldmicch=%d (only 2 or 4)", arg);
        }
    }
    if (!ch && nhltCh) { ch = nhltCh; dmicSource = "NHLT"; }
    if (!ch) { ch = 4; dmicSource = "default (no usable NHLT)"; }
    ipc = (ch == 2) ? &kPipe2ch : &kPipe4ch;
    dmicChannels = ipc->channels;
    LOG("dmic: %u ch pipeline (%s), captured on %s", dmicChannels, dmicSource, ipc->source);
}

void TGLDmic::capFail(const char *what, uint32_t value) {
    snprintf(captureResult, sizeof(captureResult), "FAIL %s (0x%08x)", what, value);
    LOG("cap: %s", captureResult);
    captureState = 2;
}

void TGLDmic::ackDspNotification() {
    uint32_t tdr = d32(DSP::HIPCTDR);
    if (!(tdr & DSP::IPC_BUSY)) return;
    LOG("cap: DSP notification 0x%08x (mbox hdr size %u cmd 0x%08x)", tdr & 0x7FFFFFFF,
        d32(DSP::MBOX_UP), d32(DSP::MBOX_UP + 4));
    dw32(DSP::HIPCTDR, tdr | DSP::IPC_BUSY);                  // W1C
    dw32(DSP::HIPCTDA, d32(DSP::HIPCTDA) | DSP::IPC_DONE);
}

// IPC3 on cAVS 2.5: message in host_box (0xA0000), HIPCIDR = BUSY; DSP sets HIPCIDA.DONE and
// writes the reply {size, cmd, error, ...} back into host_box.
bool TGLDmic::ipcSend(const uint32_t *msg, uint32_t words, uint32_t *reply, uint32_t replyWords) {
    if (d32(DSP::HIPCIDR) & DSP::IPC_BUSY) { LOG("cap: IPC channel busy before send"); return false; }
    for (uint32_t i = 0; i < words; i++) dw32(DSP::MBOX_DOWN + 4 * i, msg[i]);
    dw32(DSP::HIPCIDR, DSP::IPC_BUSY);
    uint32_t a = 0;
    for (uint32_t t = 0; t < 500000; t += 10) {
        a = d32(DSP::HIPCIDA);
        if (a & DSP::IPC_DONE) break;
        ackDspNotification();
        IODelay(10);
    }
    if (!(a & DSP::IPC_DONE)) { LOG("cap: IPC 0x%08x timeout, HIPCIDR 0x%08x", msg[1], d32(DSP::HIPCIDR)); return false; }
    uint32_t rsize = d32(DSP::MBOX_DOWN), rcmd = d32(DSP::MBOX_DOWN + 4);
    int32_t err = (int32_t)d32(DSP::MBOX_DOWN + 8);
    for (uint32_t i = 0; reply && i < replyWords; i++) reply[i] = d32(DSP::MBOX_DOWN + 4 * i);
    dw32(DSP::HIPCIDA, a | DSP::IPC_DONE);                    // W1C
    if (err < 0) { LOG("cap: IPC 0x%08x -> error %d (reply size %u cmd 0x%08x)", msg[1], err, rsize, rcmd); return false; }
    return true;
}

bool TGLDmic::replay(const IpcMsg *list, uint32_t count, const char *phase) {
    for (uint32_t i = 0; i < count; i++) {
        const IpcMsg &m = list[i];
        uint32_t buf[64];
        if (m.words > COUNT(buf)) { capFail("IPC message too large", m.words); return false; }
        memcpy(buf, m.data, m.words * 4);
        uint32_t reply[5] {};
        if (m.patch == kPatchPcmParams) {
            buf[kPcmParamPhy] = (uint32_t)ptBuf.iova;
            buf[kPcmParamPages] = PAGES;
            buf[kPcmParamSize] = RING_BYTES;
            buf[kPcmParamTagCh] = (dmicChannels << 16) | capStreamTag;
            buf[kPcmParamPeriod] = PERIOD_BYTES;
        }
        if (!ipcSend(buf, m.words, reply, 5)) {
            char what[48]; snprintf(what, sizeof(what), "%s[%u] %s", phase, i, m.name);
            capFail(what, m.cmd); return false;
        }
        if (m.patch == kPatchPcmParams) LOG("cap: PCM_PARAMS ok, comp %u posn_offset 0x%x", reply[3], reply[4]);
    }
    LOG("cap: %s: %u IPC messages ok", phase, count);
    return true;
}

bool TGLDmic::prepareCaptureStream(uint32_t idx, uint32_t tag) {
    uint32_t sd = sdOffset(idx);
    // page table for the DSP host component: 20-bit PFNs packed 2.5 bytes each (snd_sof_create_page_table)
    uint8_t *pt = ptBuf.virt;
    for (uint32_t i = 0; i < PAGES; i++) {
        uint32_t pfn = (uint32_t)((ringBuf.iova + i * 4096ULL) >> 12);
        uint8_t *p = pt + ((5 * i) >> 1);
        uint32_t v = (i & 1) ? ((p[0] & 0xF) | (pfn << 4)) : pfn;
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    }
    struct Bdle { uint32_t lo, hi, len, ioc; };
    auto bdl = reinterpret_cast<Bdle *>(capBdl.virt);
    for (uint32_t i = 0; i < RING_BYTES / PERIOD_BYTES; i++) {
        uint64_t a = ringBuf.iova + i * PERIOD_BYTES;
        bdl[i] = { (uint32_t)a, (uint32_t)(a >> 32), PERIOD_BYTES, 0 };
    }
    ptBuf.sync(); capBdl.sync();

    w32(ppOffset + HDA::PP_PPCTL, r32(ppOffset + HDA::PP_PPCTL) | (1u << idx));   // decouple -> DSP
    if (spbOffset) w32(spbOffset + SPB_SPBFCCTL, r32(spbOffset + SPB_SPBFCCTL) & ~(1u << idx));  // no SPIB for capture
    if (!streamStopReset(sd)) { capFail("capture stream reset", r32(sd)); return false; }
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, (r32(sd) & ~SD_TAG_MASK) | (tag << HDA::SD_CTL_TAG_SHIFT));
    w32(sd + HDA::SD_CBL, RING_BYTES);
    w16(sd + HDA::SD_FMT, (uint16_t)(FMT_48K_32BIT | (dmicChannels - 1)));
    w16(sd + HDA::SD_LVI, RING_BYTES / PERIOD_BYTES - 1);
    w32(sd + HDA::SD_BDPL, (uint32_t)capBdl.iova);
    w32(sd + HDA::SD_BDPU, (uint32_t)(capBdl.iova >> 32));
    LOG("cap: stream %u (sd 0x%x) tag %u ring %u B @iova 0x%llx, page table @0x%llx, bdl @0x%llx", idx, sd, tag,
        RING_BYTES, (unsigned long long)ringBuf.iova, (unsigned long long)ptBuf.iova, (unsigned long long)capBdl.iova);
    return true;
}

bool TGLDmic::ensureCaptureBuffers() {
    if (ringBuf.mem && ptBuf.mem && capBdl.mem) return true;
    releaseCaptureBuffers();
    if (ringBuf.alloc(pci, RING_BYTES) && ptBuf.alloc(pci, 4096) && capBdl.alloc(pci, 4096)) return true;
    releaseCaptureBuffers();
    return false;
}

void TGLDmic::releaseCaptureBuffers() {
    ringBuf.release(); ptBuf.release(); capBdl.release();
}

// DSP still running SOF? (an HDA driver reset or D3 would have powered the core off). Logs the reason.
bool TGLDmic::dspAlive(bool logReason) {
    const char *why = nullptr;
    uint32_t cs = 0;
    if (!dsp) why = "BAR4 not mapped";
    else if (bootState != 1) why = "never booted";
    else if (!memoryDecoded()) why = "PCI memory decoding off";
    else if (!inD0()) why = "controller not in D0";
    else {
        cs = d32(DSP::ADSPCS);
        if (cs == 0xFFFFFFFF) why = "ADSPCS reads all-ones";
        else if (!(cs & (1u << 24))) why = "core0 not powered (CPA=0)";
        else if (cs & (1u << 8)) why = "core0 stalled";
        else if (cs & 1u) why = "core0 in reset";
    }
    if (why && logReason) LOG("svc: DSP not alive: %s (ADSPCS 0x%08x, GCTL 0x%08x)", why, cs, hda ? r32(HDA::GCTL) : 0);
    return !why;
}

// Host stream prepare + IPC setup + host DMA run + IPC trigger start (hda_dsp_pcm_hw_params/trigger order)
bool TGLDmic::pipelineStart() {
    if (streaming) return true;
    const uint32_t idx = capInIndex, sd = sdOffset(idx);
    // Refuse only if the HDA driver itself is running this descriptor right now (RUN set while it is
    // still coupled to the link). A past observation is not enough: it may have been our own stream.
    if ((r32(sd) & HDA::SD_CTL_RUN) && !(r32(ppOffset + HDA::PP_PPCTL) & (1u << idx))) {
        char failMsg[64];
        snprintf(failMsg, sizeof(failMsg), "input stream %u in use by HDA driver", idx);
        capFail(failMsg, idx); return false;
    }
    everRunning &= ~(1u << idx);
    if (!ensureCaptureBuffers()) { capFail("DMA alloc", 0); return false; }
    bzero(ringBuf.virt, ringBuf.size);
    if (!prepareCaptureStream(idx, capStreamTag)) return false;
    if (!ipc) selectDmicPipeline();
    if (!replay(ipc->setup, ipc->setupCount, "setup")) { pipelineStop(); return false; }
    setupDone = true;
    w32(sd, r32(sd) | HDA::SD_CTL_RUN | SD_CTL_INT_MASK);
    uint32_t v;
    bool fifoReady = pollHda(sd, 0x20000000, 0x20000000, 5000, &v);
    LOG("cap: host DMA running, CTL 0x%08x (FIFORDY %s)", v, fifoReady ? "yes" : "no");
    if (!replay(ipc->start, ipc->startCount, "start")) { pipelineStop(); return false; }
    triggered = true;
    streaming = true;
    return true;
}

// IPC trigger stop, host DMA stop, PCM free, free widgets, release the descriptor (hda_dsp_pcm_trigger/hw_free)
void TGLDmic::pipelineStop() {
    const uint32_t idx = capInIndex, sd = sdOffset(idx);
    if (triggered && dsp && ipc) replay(ipc->stop, 1, "stop");
    w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
    pollHda(sd, HDA::SD_CTL_RUN, 0, 5000);
    if (setupDone && dsp && ipc) {
        if (triggered) replay(ipc->stop + 1, ipc->stopCount - 1, "pcm-free");
        replay(ipc->teardown, ipc->teardownCount, "teardown");
    }
    streamStopReset(sd);
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, r32(sd) & ~SD_TAG_MASK);
    w32(ppOffset + HDA::PP_PPCTL, r32(ppOffset + HDA::PP_PPCTL) & ~(1u << idx));
    triggered = setupDone = streaming = false;
}

// System sleep / parent power-down while streaming: the DSP will lose state, so stop cleanly.
IOReturn TGLDmic::gatedPowerLost(OSObject *o, void *, void *, void *, void *) {
    auto self = OSDynamicCast(TGLDmic, o);
    if (!self) return kIOReturnBadArgument;
    if (self->streaming || self->setupDone) {
        self->LOG("svc: power going down while streaming - stopping pipeline");
        self->pipelineStop();
        self->corePowerDown();
        self->gating(true);
    }
    return kIOReturnSuccess;
}

bool TGLDmic::holdPower() {
    if (pmJoined && pmState != 1) changePowerStateTo(1);
    for (uint32_t t = 0; t < 3000; t += 10) {
        if (memoryDecoded() && inD0() && r32(HDA::GCTL) != 0xFFFFFFFF && (r32(HDA::GCTL) & 1)) {
            if (t) LOG("svc: controller usable after %u ms (HDA driver power state %u)", t, pmParent ? (unsigned)pmParent->getPowerState() : 0u);
            return true;
        }
        IOSleep(10);
    }
    LOG("svc: controller still not usable after 3 s (CMD 0x%04x D0 %d GCTL 0x%08x, HDA driver power state %u, ours %lu)",
        pci->configRead16(kIOPCIConfigCommand), inD0(), memoryDecoded() ? r32(HDA::GCTL) : 0,
        pmParent ? (unsigned)pmParent->getPowerState() : 0u, pmState);
    return false;
}

void TGLDmic::releasePower() {
    if (pmJoined) changePowerStateTo(0);
}

bool TGLDmic::captureTest() {
    captureState = 0;
    LOG("cap: === phase 3 DMIC capture test ===");
    const uint32_t idx = capInIndex, dpib = VS_DPIB_BASE + VS_DPIB_STRIDE * idx;
    if (!ipc) selectDmicPipeline();
    const uint32_t nch = dmicChannels, FRAME_BYTES = srcFrameBytes();
    const uint32_t SNAPSHOT_BYTES = SNAPSHOT_FRAMES * FRAME_BYTES;
    bzero(chPeak, sizeof(chPeak)); bzero(chMeanAbs, sizeof(chMeanAbs));
    OSSafeReleaseNULL(capSnap);
    capSnap = OSData::withCapacity(SNAPSHOT_BYTES);
    bool ok = false;
    do {
        if (!capSnap) { capFail("snapshot alloc", 0); break; }
        if (!pipelineStart()) break;

        // Stream CAPTURE_SECONDS s (after 0.25 s warm-up). Keep the loudest 1-s block as the snapshot
        // and a per-100 ms mean |x| envelope (ch0) for the whole run.
        uint8_t *block = static_cast<uint8_t *>(IOMalloc(SNAPSHOT_BYTES));
        if (!block) { capFail("block alloc", 0); break; }
        capSnap->appendByte(0, SNAPSHOT_BYTES);                 // fixed-size, overwritten with the loudest block
        uint8_t *snapBytes = static_cast<uint8_t *>(const_cast<void *>(capSnap->getBytesNoCopy()));
        uint32_t blockLen = 0;
        uint64_t bestEnergy = 0, blockEnergy = 0, envAcc = 0, total = 0;
        uint32_t envFrames = 0, envCount = 0, blocks = 0, bestBlock = 0;
        uint32_t readPos = r32(dpib) % RING_BYTES, skip = 12000 * FRAME_BYTES, waited = 0;
        const uint32_t limitMs = CAPTURE_SECONDS * 1000 + 2000;
        while (blocks < CAPTURE_SECONDS && waited < limitMs) {
            IOSleep(10); waited += 10;
            uint32_t pos = r32(dpib) % RING_BYTES;
            ringBuf.invalidate();
            uint32_t avail = (pos + RING_BYTES - readPos) % RING_BYTES;
            avail -= avail % FRAME_BYTES;
            total += avail;
            while (avail && blocks < CAPTURE_SECONDS) {
                uint32_t chunk = min(avail, RING_BYTES - readPos);
                if (skip) { uint32_t s = min(chunk, skip); skip -= s; readPos = (readPos + s) % RING_BYTES; avail -= s; continue; }
                uint32_t take = min(chunk, SNAPSHOT_BYTES - blockLen);
                const int32_t *smp = reinterpret_cast<const int32_t *>(ringBuf.virt + readPos);
                for (uint32_t f = 0; f < take / FRAME_BYTES; f++) {
                    int64_t x = smp[f * nch]; uint64_t a = (uint64_t)(x < 0 ? -x : x);
                    blockEnergy += a >> 8; envAcc += a;
                    if (++envFrames == ENV_STEP_FRAMES) {
                        if (envCount < kEnvMax) env[envCount++] = (uint32_t)(envAcc / envFrames);
                        envAcc = 0; envFrames = 0;
                    }
                }
                memcpy(block + blockLen, ringBuf.virt + readPos, take);
                blockLen += take;
                readPos = (readPos + take) % RING_BYTES; avail -= take;
                if (blockLen == SNAPSHOT_BYTES) {
                    if (blockEnergy > bestEnergy || blocks == 0) {
                        bestEnergy = blockEnergy; bestBlock = blocks;
                        memcpy(snapBytes, block, SNAPSHOT_BYTES);
                    }
                    blocks++; blockEnergy = 0; blockLen = 0;
                }
            }
        }
        IOFree(block, SNAPSHOT_BYTES);
        envLen = envCount;
        capBytes = total;
        LOG("cap: streamed %llu bytes in %u ms (expect ~%u B/ms, %u ch), %u x 1 s blocks, loudest = block %u",
            (unsigned long long)total, waited, 48 * FRAME_BYTES, nch, blocks, bestBlock);
        for (uint32_t i = 0; i + 10 <= envCount; i += 10)
            LOG("cap: env %us: %u %u %u %u %u %u %u %u %u %u", i / 10, env[i], env[i+1], env[i+2], env[i+3], env[i+4],
                env[i+5], env[i+6], env[i+7], env[i+8], env[i+9]);
        if (blocks < 1) { capFail("DMA position did not advance", r32(dpib)); break; }

        // per-channel peak and mean |x| over the snapshot (S32_LE, nch interleaved; unused slots stay 0)
        auto s = static_cast<const int32_t *>(capSnap->getBytesNoCopy());
        uint32_t frames = capSnap->getLength() / FRAME_BYTES;
        uint64_t sum[kMaxDmicChannels] {};
        for (uint32_t f = 0; f < frames; f++)
            for (uint32_t c = 0; c < nch; c++) {
                int64_t x = s[f * nch + c]; uint32_t a = (uint32_t)(x < 0 ? -x : x);
                if (a > chPeak[c]) chPeak[c] = a;
                sum[c] += a;
            }
        for (uint32_t c = 0; c < nch; c++) chMeanAbs[c] = frames ? (uint32_t)(sum[c] / frames) : 0;
        LOG("cap: peak |x| ch0 %u ch1 %u ch2 %u ch3 %u (full scale 2147483647)", chPeak[0], chPeak[1], chPeak[2], chPeak[3]);
        LOG("cap: mean |x| ch0 %u ch1 %u ch2 %u ch3 %u", chMeanAbs[0], chMeanAbs[1], chMeanAbs[2], chMeanAbs[3]);
        bool signal = chMeanAbs[0] || chMeanAbs[1] || chMeanAbs[2] || chMeanAbs[3];
        snprintf(captureResult, sizeof(captureResult), signal ? "OK %u frames, signal present" : "DMA OK but all-zero samples",
                 frames);
        captureState = signal ? 1 : 2;
        ok = signal;
    } while (false);

    pipelineStop();
    if (!serviceMode) releaseCaptureBuffers();
    LOG("cap: done: %s", captureResult);
    return ok;
}
