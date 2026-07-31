#pragma once
// Copyright 2026 Justin
//
// USB mass-storage: plug the device into a computer and the internal FFat
// partition (history CSVs, logs, config) mounts as a read-only flash drive.
//
// Access model — exclusive handoff, never shared:
//   The wear-levelling layer must have exactly one owner. Normally that owner
//   is FFat (firmware writing samples). On the host's FIRST sector read the
//   firmware unmounts its filesystem and takes a private wl handle for USB;
//   on SCSI eject (or a safety idle timeout) it hands back and remounts.
//   While the host holds the disk, history/log writes no-op gracefully — the
//   storage API already degrades when unmounted, and you are standing at the
//   device anyway.
//
// The drive is read-only at the SCSI level: a stray host write can never
// corrupt the filesystem (owner requirement: storage must be un-weddable).

#include "feature_flags.h"

#if FEATURE_USB_MSC

// Register geometry/callbacks. Call once from setup, after storage is up.
void usb_msc_begin();

// Safety net: reverts the handoff when the host vanished without ejecting.
// Call from the main loop.
void usb_msc_tick();

// True while the host owns the disk (storage writes are paused).
bool usb_msc_host_active();

#else

inline void usb_msc_begin() {}
inline void usb_msc_tick() {}
inline bool usb_msc_host_active() { return false; }

#endif  // FEATURE_USB_MSC
