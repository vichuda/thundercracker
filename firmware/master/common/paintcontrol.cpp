/*
 * Thundercracker Firmware -- Confidential, not for redistribution.
 * Copyright <c> 2012 Sifteo, Inc. All rights reserved.
 */

#include "paintcontrol.h"
#include "tasks.h"
#include "radio.h"
#include "vram.h"
#include "cube.h"
#include "systime.h"

#ifdef SIFTEO_SIMULATOR
#   include "system.h"
#   include "system_mc.h"
#   define PAINT_LOG(_x)    do { if (SystemMC::getSystem()->opt_paintTrace) { LOG(_x); }} while (0)
#else
#   define PAINT_LOG(_x)
#endif

#define LOG_PREFIX  "PAINT[%d]: %6u.%03us [+%4ums] pend=%-3d flags=%08x[%c%c%c%c%c] vf=%02x[%c%c] ack=%02x lock=%08x cm16=%08x  "
#define LOG_PARAMS  cube->id(), \
                    unsigned(SysTime::ticks() / SysTime::sTicks(1)), \
                    unsigned((SysTime::ticks() % SysTime::sTicks(1)) / SysTime::msTicks(1)), \
                    unsigned((SysTime::ticks() - paintTimestamp) / SysTime::msTicks(1)), \
                    pendingFrames, \
                    vbuf ? vbuf->flags : 0xFFFFFFFF, \
                        (vbuf && (vbuf->flags & _SYS_VBF_FLAG_SYNC))        ? 's' : ' ', \
                        (vbuf && (vbuf->flags & _SYS_VBF_TRIGGER_ON_FLUSH)) ? 't' : ' ', \
                        (vbuf && (vbuf->flags & _SYS_VBF_SYNC_ACK))         ? 'a' : ' ', \
                        (vbuf && (vbuf->flags & _SYS_VBF_DIRTY_RENDER))     ? 'R' : ' ', \
                        (vbuf && (vbuf->flags & _SYS_VBF_NEED_PAINT))       ? 'P' : ' ', \
                    vbuf ? getFlags(vbuf): 0xFF, \
                        (vbuf && (getFlags(vbuf) & _SYS_VF_TOGGLE))         ? 't' : ' ', \
                        (vbuf && (getFlags(vbuf) & _SYS_VF_CONTINUOUS))     ? 'C' : ' ', \
                    cube->getLastFrameACK(), \
                    vbuf ? vbuf->lock : 0xFFFFFFFF, \
                    vbuf ? vbuf->cm16 : 0xFFFFFFFF


/*
 * This object manages the somewhat complex asynchronous rendering pipeline.
 * We try to balance fast asynchronous rendering with slower but more deliberate
 * synchronous rendering.
 *
 * Here be dragons...?
 */

/*
 * System-owned VideoBuffer flag bits.
 *
 * Some of these flags are public, defined in the ABI. So far, the range
 * 0x0000FFFF is tentatively defined for public bits, while 0xFFFF0000 is
 * for these private bits. We make no guarantee about the meaning of these
 * bits, except that it's safe to initialize them to zero.
 */

// Public bits, defined in abi.h
//      _SYS_VBF_NEED_PAINT         (1 << 0)    // Request a paint operation

#define _SYS_VBF_DIRTY_RENDER       (1 << 16)   // Still rendering changed VRAM
#define _SYS_VBF_SYNC_ACK           (1 << 17)   // Frame ACK is synchronous (pendingFrames is 0 or 1)
#define _SYS_VBF_TRIGGER_ON_FLUSH   (1 << 18)   // Trigger a paint from vramFlushed()
#define _SYS_VBF_FLAG_SYNC          (1 << 19)   // This VideoBuffer has sync'ed flags with the cube


/*
 * Frame rate control parameters:
 *
 * fpsLow --
 *    "Minimum" frame rate. If we're waiting more than this long
 *    for a frame to render, give up. Prevents us from getting wedged
 *    if a cube stops responding.
 *
 * fpsHigh --
 *    Maximum frame rate. Paint will always block until at least this
 *    long since the previous frame, in order to provide a global rate
 *    limit for the whole app.
 *
 * fpMax --
 *    Maximum number of pending frames to track in continuous mode.
 *    If we hit this limit, Paint() calls will block.
 *
 * fpMin --
 *    Minimum number of pending frames to track in continuous mode.
 *    If we go below this limit, we'll start ignoring acknowledgments.
 */

static const SysTime::Ticks fpsLow = SysTime::hzTicks(4);
static const SysTime::Ticks fpsHigh = SysTime::hzTicks(60);
static const int8_t fpMax = 5;
static const int8_t fpMin = -8;


void PaintControl::waitForPaint(CubeSlot *cube)
{
    /*
     * Wait until we're allowed to do another paint. Since our
     * rendering is usually not fully synchronous, this is not nearly
     * as strict as waitForFinish()!
     */

    _SYSVideoBuffer *vbuf = cube->getVBuf();

    PAINT_LOG((LOG_PREFIX "+waitForPaint\n", LOG_PARAMS));

    SysTime::Ticks now;
    for (;;) {
        Atomic::Barrier();
        now = SysTime::ticks();

        // Watchdog expired? Give up waiting.
        if (now > paintTimestamp + fpsLow) {
            PAINT_LOG((LOG_PREFIX "waitForPaint, TIMED OUT\n", LOG_PARAMS));
            break;
        }

        // Wait for minimum frame rate AND for pending renders
        if (now > paintTimestamp + fpsHigh && pendingFrames <= fpMax)
            break;

        Tasks::work();
        Radio::halt();
    }

    /*
     * Can we opportunistically regain our synchronicity here?
     */

    if (vbuf && canMakeSynchronous(vbuf, now)) {
        makeSynchronous(cube, vbuf);
        pendingFrames = 0;
    }

    PAINT_LOG((LOG_PREFIX "-waitForPaint\n", LOG_PARAMS));
}

void PaintControl::triggerPaint(CubeSlot *cube, SysTime::Ticks now)
{
    _SYSVideoBuffer *vbuf = cube->getVBuf();

    /*
     * We must always update paintTimestamp, even if this turned out
     * to be a no-op. An application which makes no changes to VRAM
     * but just calls paint() in a tight loop should iterate at the
     * 'fastPeriod' defined above.
     */
    paintTimestamp = now;

    if (!vbuf)
        return;

    int32_t pending = Atomic::Load(pendingFrames);
    int32_t newPending = pending;

    PAINT_LOG((LOG_PREFIX "+triggerPaint\n", LOG_PARAMS));

    bool needPaint = (vbuf->flags & _SYS_VBF_NEED_PAINT) != 0;
    Atomic::And(vbuf->flags, ~_SYS_VBF_NEED_PAINT);

    /*
     * Keep pendingFrames above the lower limit. We make this
     * adjustment lazily, rather than doing it from inside the
     * ISR.
     */

    if (pending < fpMin)
        newPending = fpMin;

    /*
     * If we're in continuous rendering, we must count every single
     * Paint invocation for the purposes of loosely matching them with
     * acknowledged frames. This isn't a strict 1:1 mapping, but
     * it's used to close the loop on repaint speed.
     */

    if (needPaint) {
        newPending++;

        /*
         * There are multiple ways to enter continuous mode: vramFlushed()
         * can do so while handling a TRIGGER_ON_FLUSH flag, if we aren't
         * sync'ed by then. But we can't rely on this as our only way to
         * start rendering. If userspace is just pumping data into a VideoBuffer
         * like mad, and we can't stream it out over the radio quite fast enough,
         * we may not get a chance to enter vramFlushed() very often.
         *
         * So, the primary method for entering continuous mode is still as a
         * result of TRIGGER_ON_FLUSH. But as a backup, we'll enter it now
         * if we see frames stacking up in newPending.
         */

        if (newPending >= fpMax && allowContinuous(cube)) {
            uint8_t vf = getFlags(vbuf);
            if (!(vf & _SYS_VF_CONTINUOUS)) {
                enterContinuous(cube, vbuf, vf);
                setFlags(vbuf, vf);
            }
            newPending = fpMax;
        }

        // When the codec calls us back in vramFlushed(), trigger a render
        if (!isContinuous(vbuf)) {
            // Trigger on the next flush
            asyncTimestamp = now;
            Atomic::Or(vbuf->flags, _SYS_VBF_TRIGGER_ON_FLUSH);

            // Provoke a VRAM flush, just in case this wasn't happening anyway.
            if (vbuf->lock == 0)
                VRAM::lock(*vbuf, _SYS_VA_FLAGS/2);
        }

        // Unleash the radio codec!
        VRAM::unlock(*vbuf);
    }

    // Atomically apply our changes to pendingFrames.
    Atomic::Add(pendingFrames, newPending - pending);

    PAINT_LOG((LOG_PREFIX "-triggerPaint\n", LOG_PARAMS));
}

void PaintControl::waitForFinish(CubeSlot *cube)
{
    /*
     * Wait until all previous rendering has finished, and all of VRAM
     * has been updated over the radio.  Does *not* wait for any
     * minimum frame rate. If no rendering is pending, we return
     * immediately.
     *
     * Requires a valid attached video buffer.
     */

    _SYSVideoBuffer *vbuf = cube->getVBuf();
    ASSERT(vbuf);

    PAINT_LOG((LOG_PREFIX "+waitForFinish\n", LOG_PARAMS));

    // Disable continuous rendering now, if it was on.
    uint8_t vf = getFlags(vbuf);
    exitContinuous(cube, vbuf, vf, SysTime::ticks());
    setFlags(vbuf, vf);

    // Things to wait for...
    const uint32_t mask = _SYS_VBF_TRIGGER_ON_FLUSH | _SYS_VBF_DIRTY_RENDER;

    for (;;) {
        uint32_t flags = Atomic::Load(vbuf->flags);
        SysTime::Ticks now = SysTime::ticks();

        // Already done, without any arm-twisting?
        if ((mask & flags) == 0)
            break;

        // Has it been a while since the last trigger?
        if (canMakeSynchronous(vbuf, now)) {
            makeSynchronous(cube, vbuf);

            if (flags & _SYS_VBF_DIRTY_RENDER) {
                // Still need a render. Re-trigger now.

                PAINT_LOG((LOG_PREFIX "waitForFinish RE-TRIGGER\n", LOG_PARAMS));
                ASSERT(!isContinuous(vbuf));

                Atomic::Or(vbuf->flags, _SYS_VBF_NEED_PAINT);
                triggerPaint(cube, now);

            } else {
                // The trigger expired, and we don't need to render. We're done.

                Atomic::And(vbuf->flags, ~_SYS_VBF_TRIGGER_ON_FLUSH);
                break;
            }
        }

        // Wait..
        Tasks::work();
        Radio::halt();
    }

    PAINT_LOG((LOG_PREFIX "-waitForFinish\n", LOG_PARAMS));
}

void PaintControl::ackFrames(CubeSlot *cube, int32_t count)
{
    /*
     * One or more frames finished rendering on the cube.
     * Use this to update our pendingFrames accumulator.
     *
     * If we are _not_ in continuous rendering mode, and
     * we have synchronized our ACK bits with the cube's
     * TOGGLE bit, this means the frame has finished
     * rendering and we can clear the 'render' dirty bit.
     */
    
    pendingFrames -= count;

    _SYSVideoBuffer *vbuf = cube->getVBuf();
    if (vbuf) {
        uint32_t vf = getFlags(vbuf);

        if ((vf & _SYS_VF_CONTINUOUS) == 0 &&
            (vbuf->flags & _SYS_VBF_SYNC_ACK) != 0) {

            // Render is clean
            Atomic::And(vbuf->flags, ~_SYS_VBF_DIRTY_RENDER);
        }

        // Too few pending frames? Disable continuous mode.
        if (pendingFrames < fpMin) {
            uint8_t vf = getFlags(vbuf);
            exitContinuous(cube, vbuf, vf, SysTime::ticks());
            setFlags(vbuf, vf);
        }

        PAINT_LOG((LOG_PREFIX "ACK(%d)\n", LOG_PARAMS, count));
    }
}

void PaintControl::vramFlushed(CubeSlot *cube)
{
    /*
     * Finished flushing VRAM out to the cubes. This is only called when
     * we've fully emptied our queue of pending radio transmissions, and
     * the cube's VRAM should match our local copy exactly.
     *
     * If we are in continuous rendering mode, this isn't really an
     * important event. But if we're in synchronous mode, this indicates
     * that everything in the VRAM dirty bit can now be tracked
     * by the RENDER dirty bit; in other words, all dirty VRAM has been
     * flushed, and we can start a clean frame rendering.
     */

    _SYSVideoBuffer *vbuf = cube->getVBuf();
    if (!vbuf)
        return;
    uint8_t vf = getFlags(vbuf);

    PAINT_LOG((LOG_PREFIX "vramFlushed\n", LOG_PARAMS));

    // We've flushed VRAM, flags are sync'ed from now on.
    Atomic::Or(vbuf->flags, _SYS_VBF_FLAG_SYNC);

    if (vbuf->flags & _SYS_VBF_TRIGGER_ON_FLUSH) {
        // Trying to trigger a render

        PAINT_LOG((LOG_PREFIX "TRIGGERING\n", LOG_PARAMS));

        if (cube->hasValidFrameACK() && (vbuf->flags & _SYS_VBF_SYNC_ACK)) {
            // We're sync'ed up. Trigger a one-shot render

            // Should never have SYNC_ACK set when in CONTINUOUS mode.
            ASSERT((vf & _SYS_VF_CONTINUOUS) == 0);

            setToggle(cube, vbuf, vf, SysTime::ticks());

        } else {
            /*
             * We're getting ahead of the cube. We'd like to trigger now, but
             * we're no longer in sync. So, enter continuous mode. This will
             * break synchronization, in the interest of keeping our speed up.
             */

             if (!(vf & _SYS_VF_CONTINUOUS))
                enterContinuous(cube, vbuf, vf);
        }

        setFlags(vbuf, vf);

        // Propagate the bits...
        Atomic::Or(vbuf->flags, _SYS_VBF_DIRTY_RENDER);
        Atomic::And(vbuf->flags, ~_SYS_VBF_TRIGGER_ON_FLUSH);
    }
}

bool PaintControl::allowContinuous(CubeSlot *cube)
{
    // Conserve cube CPU time during asset loading; don't use continuous rendering.
    return !cube->isAssetLoading();
}

void PaintControl::enterContinuous(CubeSlot *cube, _SYSVideoBuffer *vbuf, uint8_t &flags)
{
    bool allowed = allowContinuous(cube);

    PAINT_LOG((LOG_PREFIX "enterContinuous, allowed=%d\n", LOG_PARAMS, allowed));

    // Entering continuous mode; all synchronization goes out the window.
    Atomic::And(vbuf->flags, ~_SYS_VBF_SYNC_ACK);
    Atomic::Or(vbuf->flags, _SYS_VBF_DIRTY_RENDER);

    if (allowed) {
        flags |= _SYS_VF_CONTINUOUS;
    } else {
        // Ugh.. can't do real synchronous rendering, but we also can't
        // use continuous rendering here. So... just flip the toggle bit
        // and hope for the best.
        flags &= ~_SYS_VF_CONTINUOUS;
        flags ^= _SYS_VF_TOGGLE;
    }
}

void PaintControl::exitContinuous(CubeSlot *cube, _SYSVideoBuffer *vbuf,
    uint8_t &flags, SysTime::Ticks timestamp)
{
    PAINT_LOG((LOG_PREFIX "exitContinuous\n", LOG_PARAMS));

    // Exiting continuous mode; treat this as the last trigger point.
    if (flags & _SYS_VF_CONTINUOUS) {
        flags &= ~_SYS_VF_CONTINUOUS;
        asyncTimestamp = timestamp;
    }
}

bool PaintControl::isContinuous(_SYSVideoBuffer *vbuf)
{
    return (getFlags(vbuf) & _SYS_VF_CONTINUOUS) != 0;
}

void PaintControl::setToggle(CubeSlot *cube, _SYSVideoBuffer *vbuf,
    uint8_t &flags, SysTime::Ticks timestamp)
{
    PAINT_LOG((LOG_PREFIX "setToggle\n", LOG_PARAMS));

    asyncTimestamp = timestamp;
    if (cube->getLastFrameACK() & 1)
        flags &= ~_SYS_VF_TOGGLE;
    else
        flags |= _SYS_VF_TOGGLE;
}

void PaintControl::makeSynchronous(CubeSlot *cube, _SYSVideoBuffer *vbuf)
{
    PAINT_LOG((LOG_PREFIX "makeSynchronous\n", LOG_PARAMS));

    pendingFrames = 0;

    // We can only enter SYNC_ACK state if we know that vbuf's flags
    // match what's on real hardware. We know this after any vramFlushed().
    if (vbuf->flags & _SYS_VBF_FLAG_SYNC)
        Atomic::Or(vbuf->flags, _SYS_VBF_SYNC_ACK);
}

bool PaintControl::canMakeSynchronous(_SYSVideoBuffer *vbuf, SysTime::Ticks timestamp)
{
    return !isContinuous(vbuf) && timestamp > asyncTimestamp + fpsLow;
}