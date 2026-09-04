/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Architecture-neutral shim for coreboot's cbmem timestamp table
 *
 * When U-Boot runs as a coreboot payload it can append entries to the
 * timestamp table coreboot left in cbmem. Those entries share coreboot's time
 * base (raw TSC relative to the table's base_time), so "cbmem -t" in the OS
 * shows firmware and payload phases on one timeline.
 *
 * Generic code cannot include <asm/arch/timestamp.h> directly - that header
 * only resolves when the SoC is coreboot - so use cb_timestamp() instead. It
 * compiles to nothing everywhere else, and because the argument is dropped
 * entirely the TS_* enum does not need to exist on other architectures.
 *
 * Copyright 2026 9elements GmbH
 */

#ifndef __CB_TIMESTAMP_H
#define __CB_TIMESTAMP_H

#if defined(CONFIG_X86) && defined(CONFIG_SYS_COREBOOT)

#include <asm/arch/timestamp.h>

/**
 * cb_timestamp() - record the current time against an ID in cbmem
 *
 * Silently does nothing if coreboot provided no timestamp table or the table
 * is full. Costs one rdtsc plus a few stores, so it is safe on a hot path.
 *
 * @id: enum timestamp_id to record
 */
#define cb_timestamp(id)	timestamp_add_now(id)

#else

#define cb_timestamp(id)	do { } while (0)

#endif

#endif /* __CB_TIMESTAMP_H */
