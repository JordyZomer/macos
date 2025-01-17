/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 *	File:	kern/gzalloc.c
 *	Author:	Derek Kumar
 *
 *	"Guard mode" zone allocator, used to trap use-after-free errors,
 *	overruns, underruns, mismatched allocations/frees, uninitialized
 *	zone element use, timing dependent races etc.
 *
 *	The allocator is configured by these boot-args:
 *	gzalloc_size=<size>: target all zones with elements of <size> bytes
 *	gzalloc_min=<size>: target zones with elements >= size
 *	gzalloc_max=<size>: target zones with elements <= size
 *      gzalloc_min/max can be specified in conjunction to target a range of
 *	sizes
 *	gzalloc_fc_size=<size>: number of zone elements (effectively page
 *	multiple sized) to retain in the free VA cache. This cache is evicted
 *	(backing pages and VA released) in a least-recently-freed fashion.
 *	Larger free VA caches allow for a longer window of opportunity to trap
 *	delayed use-after-free operations, but use more memory.
 *	-gzalloc_wp: Write protect, rather than unmap, freed allocations
 *	lingering in the free VA cache. Useful to disambiguate between
 *	read-after-frees/read overruns and writes. Also permits direct inspection
 *	of the freed element in the cache via the kernel debugger. As each
 *	element has a "header" (trailer in underflow detection mode), the zone
 *	of origin of the element can be easily determined in this mode.
 *	-gzalloc_uf_mode: Underflow detection mode, where the guard page
 *	adjoining each element is placed *before* the element page rather than
 *	after. The element is also located at the top of the page, rather than
 *	abutting the bottom as with the standard overflow detection mode.
 *	-gzalloc_noconsistency: disable consistency checks that flag mismatched
 *	frees, corruptions of the header/trailer signatures etc.
 *	-nogzalloc_mode: Disables the guard mode allocator. The DEBUG kernel
 *	enables the guard allocator for zones sized 1K (if present) by
 *	default, this option can disable that behaviour.
 *	gzname=<name> target a zone by name. Can be coupled with size-based
 *	targeting. Naming conventions match those of the zlog boot-arg, i.e.
 *	"a period in the logname will match a space in the zone name"
 *	-gzalloc_no_dfree_check Eliminate double free checks
 *	gzalloc_zscale=<value> specify size multiplier for the dedicated gzalloc submap
 */

#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <mach/kern_return.h>
#include <mach/machine/vm_types.h>
#include <mach_debug/zone_info.h>
#include <mach/vm_map.h>

#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/sched.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/zalloc_internal.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>

#include <pexpert/pexpert.h>

#include <machine/machparam.h>

#include <libkern/OSDebug.h>
#include <libkern/OSAtomic.h>
#include <sys/kdebug.h>

boolean_t gzalloc_mode = FALSE;
uint32_t pdzalloc_count, pdzfree_count;

#define GZALLOC_MIN_DEFAULT (1024)
#define GZDEADZONE ((zone_t) 0xDEAD201E)
#define GZALLOC_SIGNATURE (0xABADCAFE)
#define GZALLOC_RESERVE_SIZE_DEFAULT (2 * 1024 * 1024)
#define GZFC_DEFAULT_SIZE (1536)

char gzalloc_fill_pattern = 0x67; /* 'g' */

uint32_t gzalloc_min = ~0U;
uint32_t gzalloc_max = 0;
uint32_t gzalloc_size = 0;
uint64_t gzalloc_allocated, gzalloc_freed, gzalloc_early_alloc, gzalloc_early_free, gzalloc_wasted;
boolean_t gzalloc_uf_mode = FALSE, gzalloc_consistency_checks = TRUE, gzalloc_dfree_check = TRUE;
vm_prot_t gzalloc_prot = VM_PROT_NONE;
uint32_t gzalloc_guard = KMA_GUARD_LAST;
uint32_t gzfc_size = GZFC_DEFAULT_SIZE;
uint32_t gzalloc_zonemap_scale = 1;

__startup_data
vm_map_size_t gzalloc_map_size = 0;
vm_map_t gzalloc_map;
struct kmem_range gzalloc_range;
vm_offset_t gzalloc_reserve;
vm_size_t gzalloc_reserve_size;

typedef struct gzalloc_header {
	zone_t gzone;
	uint32_t  gzsize;
	uint32_t  gzsig;
} gzhdr_t;

#define GZHEADER_SIZE (sizeof(gzhdr_t))

extern zone_t vm_page_zone;

static zone_t gztrackzone = NULL;
static char gznamedzone[MAX_ZONE_NAME] = "";

boolean_t
gzalloc_enabled(void)
{
	return gzalloc_mode;
}

void
gzalloc_zone_init(zone_t z)
{
	if (gzalloc_mode == 0) {
		return;
	}

	bzero(&z->gz, sizeof(z->gz));

	if (track_this_zone(z->z_name, gznamedzone)) {
		gztrackzone = z;
	}

	z->z_gzalloc_tracked = (z == gztrackzone) ||
	    ((zone_elem_size(z) >= gzalloc_min) && (zone_elem_size(z) <= gzalloc_max));

	if (gzfc_size && z->z_gzalloc_tracked) {
		vm_size_t gzfcsz = round_page(sizeof(*z->gz.gzfc) * gzfc_size);

		/* If the VM/kmem system aren't yet configured, carve
		 * out the free element cache structure directly from the
		 * gzalloc_reserve supplied by the pmap layer.
		 */
		if (__improbable(startup_phase < STARTUP_SUB_KMEM)) {
			if (gzalloc_reserve_size < gzfcsz) {
				panic("gzalloc reserve exhausted");
			}

			z->gz.gzfc = (vm_offset_t *)gzalloc_reserve;
			gzalloc_reserve += gzfcsz;
			gzalloc_reserve_size -= gzfcsz;
			bzero(z->gz.gzfc, gzfcsz);
		} else {
			kernel_memory_allocate(kernel_map,
			    (vm_offset_t *)&z->gz.gzfc, gzfcsz, 0,
			    KMA_NOFAIL | KMA_KOBJECT | KMA_ZERO, VM_KERN_MEMORY_OSFMK);
		}
	}
}

/* Called by zdestroy() to dump the free cache elements so the zone count can drop to zero. */
void
gzalloc_empty_free_cache(zone_t zone)
{
	int freed_elements = 0;
	vm_offset_t free_addr = 0;
	vm_offset_t rounded_size = round_page(zone_elem_size(zone) + GZHEADER_SIZE);
	vm_offset_t gzfcsz = round_page(sizeof(*zone->gz.gzfc) * gzfc_size);
	vm_offset_t gzfc_copy;

	assert(zone->z_gzalloc_tracked); // the caller is responsible for checking

	kernel_memory_allocate(kernel_map, &gzfc_copy, gzfcsz, 0,
	    KMA_NOFAIL, VM_KERN_MEMORY_OSFMK);

	/* Reset gzalloc_data. */
	zone_lock(zone);
	memcpy((void *)gzfc_copy, (void *)zone->gz.gzfc, gzfcsz);
	bzero((void *)zone->gz.gzfc, gzfcsz);
	zone->gz.gzfc_index = 0;
	zone_unlock(zone);

	/* Free up all the cached elements. */
	for (uint32_t index = 0; index < gzfc_size; index++) {
		free_addr = ((vm_offset_t *)gzfc_copy)[index];
		if (free_addr && kmem_range_contains(&gzalloc_range, free_addr)) {
			kmem_free(gzalloc_map, free_addr, rounded_size + PAGE_SIZE);
			OSAddAtomic64((SInt32)rounded_size, &gzalloc_freed);
			OSAddAtomic64(-((SInt32) (rounded_size - zone_elem_size(zone))), &gzalloc_wasted);

			freed_elements++;
		}
	}
	/*
	 * TODO: Consider freeing up zone->gz.gzfc as well if it didn't come from the gzalloc_reserve pool.
	 * For now we're reusing this buffer across zdestroy's. We would have to allocate it again on a
	 * subsequent zinit() as well.
	 */

	/* Decrement zone counters. */
	zone_lock(zone);
	zone->z_elems_free += freed_elements;
	zone->z_wired_cur -= freed_elements;
	zone_unlock(zone);

	kmem_free(kernel_map, gzfc_copy, gzfcsz);
}

__startup_func
static void
gzalloc_configure(void)
{
#if !KASAN_ZALLOC
	char temp_buf[16];

	if (PE_parse_boot_argn("-gzalloc_mode", temp_buf, sizeof(temp_buf))) {
		gzalloc_mode = TRUE;
		gzalloc_min = GZALLOC_MIN_DEFAULT;
		gzalloc_max = ~0U;
	}

	if (PE_parse_boot_argn("gzalloc_min", &gzalloc_min, sizeof(gzalloc_min))) {
		gzalloc_mode = TRUE;
		gzalloc_max = ~0U;
	}

	if (PE_parse_boot_argn("gzalloc_max", &gzalloc_max, sizeof(gzalloc_max))) {
		gzalloc_mode = TRUE;
		if (gzalloc_min == ~0U) {
			gzalloc_min = 0;
		}
	}

	if (PE_parse_boot_argn("gzalloc_size", &gzalloc_size, sizeof(gzalloc_size))) {
		gzalloc_min = gzalloc_max = gzalloc_size;
		gzalloc_mode = TRUE;
	}

	(void)PE_parse_boot_argn("gzalloc_fc_size", &gzfc_size, sizeof(gzfc_size));

	if (PE_parse_boot_argn("-gzalloc_wp", temp_buf, sizeof(temp_buf))) {
		gzalloc_prot = VM_PROT_READ;
	}

	if (PE_parse_boot_argn("-gzalloc_uf_mode", temp_buf, sizeof(temp_buf))) {
		gzalloc_uf_mode = TRUE;
		gzalloc_guard = KMA_GUARD_FIRST;
	}

	if (PE_parse_boot_argn("-gzalloc_no_dfree_check", temp_buf, sizeof(temp_buf))) {
		gzalloc_dfree_check = FALSE;
	}

	(void) PE_parse_boot_argn("gzalloc_zscale", &gzalloc_zonemap_scale, sizeof(gzalloc_zonemap_scale));

	if (PE_parse_boot_argn("-gzalloc_noconsistency", temp_buf, sizeof(temp_buf))) {
		gzalloc_consistency_checks = FALSE;
	}

	if (PE_parse_boot_argn("gzname", gznamedzone, sizeof(gznamedzone))) {
		gzalloc_mode = TRUE;
	}
#if DEBUG
	if (gzalloc_mode == FALSE) {
		gzalloc_min = 1024;
		gzalloc_max = 1024;
		strlcpy(gznamedzone, "pmap", sizeof(gznamedzone));
		gzalloc_prot = VM_PROT_READ;
		gzalloc_mode = TRUE;
	}
#endif
	if (PE_parse_boot_argn("-nogzalloc_mode", temp_buf, sizeof(temp_buf))) {
		gzalloc_mode = FALSE;
	}

	if (gzalloc_mode) {
		gzalloc_reserve_size = GZALLOC_RESERVE_SIZE_DEFAULT;
		gzalloc_reserve = (vm_offset_t) pmap_steal_memory(gzalloc_reserve_size);
	}
#endif
}
STARTUP(PMAP_STEAL, STARTUP_RANK_FIRST, gzalloc_configure);

KMEM_RANGE_REGISTER_DYNAMIC(gzalloc_map, &gzalloc_range, ^{
	if (gzalloc_mode) {
	        gzalloc_map_size = (zone_map_size * gzalloc_zonemap_scale);
	}
	return gzalloc_map_size;
});

void
gzalloc_init(void)
{
	if (gzalloc_mode) {
		gzalloc_map = kmem_suballoc(kernel_map, &gzalloc_range.min_address,
		    gzalloc_map_size, VM_MAP_CREATE_DEFAULT,
		    VM_FLAGS_FIXED_RANGE_SUBALLOC, KMS_PERMANENT | KMS_NOFAIL,
		    VM_KERN_MEMORY_ZONE).kmr_submap;
	}
}

vm_offset_t
gzalloc_alloc(zone_t zone, zone_stats_t zstats, zalloc_flags_t flags)
{
	vm_offset_t addr = 0;

	assert(zone->z_gzalloc_tracked); // the caller is responsible for checking

	if (get_preemption_level() != 0) {
		if (flags & Z_NOWAIT) {
			return 0;
		}
		pdzalloc_count++;
	}

	bool kmem_ready = (startup_phase >= STARTUP_SUB_KMEM);
	vm_offset_t rounded_size = round_page(zone_elem_size(zone) + GZHEADER_SIZE);
	vm_offset_t residue = rounded_size - zone_elem_size(zone);
	vm_offset_t gzaddr = 0;
	gzhdr_t *gzh, *gzhcopy = NULL;
	bool new_va = false;

	if (!kmem_ready || (vm_page_zone == ZONE_NULL)) {
		/* Early allocations are supplied directly from the
		 * reserve.
		 */
		if (gzalloc_reserve_size < (rounded_size + PAGE_SIZE)) {
			panic("gzalloc reserve exhausted");
		}
		gzaddr = gzalloc_reserve;
		/* No guard page for these early allocations, just
		 * waste an additional page.
		 */
		gzalloc_reserve += rounded_size + PAGE_SIZE;
		gzalloc_reserve_size -= rounded_size + PAGE_SIZE;
		OSAddAtomic64((SInt32) (rounded_size), &gzalloc_early_alloc);
	} else {
		kernel_memory_allocate(gzalloc_map,
		    &gzaddr, rounded_size + PAGE_SIZE, 0,
		    KMA_NOFAIL | KMA_ZERO | KMA_KOBJECT | gzalloc_guard,
		    VM_KERN_MEMORY_OSFMK);
		new_va = true;
	}

	if (gzalloc_uf_mode) {
		gzaddr += PAGE_SIZE;
		/* The "header" becomes a "footer" in underflow
		 * mode.
		 */
		gzh = (gzhdr_t *) (gzaddr + zone_elem_size(zone));
		addr = gzaddr;
		gzhcopy = (gzhdr_t *) (gzaddr + rounded_size - sizeof(gzhdr_t));
	} else {
		gzh = (gzhdr_t *) (gzaddr + residue - GZHEADER_SIZE);
		addr = (gzaddr + residue);
	}

	/*
	 * All zone allocations are always zeroed
	 */
	bzero((void *)gzaddr, rounded_size);

	gzh->gzone = (kmem_ready && vm_page_zone) ? zone : GZDEADZONE;
	gzh->gzsize = (uint32_t)zone_elem_size(zone);
	gzh->gzsig = GZALLOC_SIGNATURE;

	/* In underflow detection mode, stash away a copy of the
	 * metadata at the edge of the allocated range, for
	 * retrieval by gzalloc_element_size()
	 */
	if (gzhcopy) {
		*gzhcopy = *gzh;
	}

	zone_lock(zone);
	assert(zone->z_self == zone);
	zone->z_elems_free--;
	if (new_va) {
		zone->z_va_cur += 1;
	}
	zone->z_wired_cur += 1;
	zpercpu_get(zstats)->zs_mem_allocated += rounded_size;
	zone_unlock(zone);

	OSAddAtomic64((SInt32) rounded_size, &gzalloc_allocated);
	OSAddAtomic64((SInt32) (rounded_size - zone_elem_size(zone)), &gzalloc_wasted);

	return addr;
}

void
gzalloc_free(zone_t zone, zone_stats_t zstats, void *addr)
{
	kern_return_t kr;

	assert(zone->z_gzalloc_tracked); // the caller is responsible for checking

	gzhdr_t *gzh;
	vm_offset_t rounded_size = round_page(zone_elem_size(zone) + GZHEADER_SIZE);
	vm_offset_t residue = rounded_size - zone_elem_size(zone);
	vm_offset_t saddr;
	vm_offset_t free_addr = 0;

	if (gzalloc_uf_mode) {
		gzh = (gzhdr_t *)((vm_offset_t)addr + zone_elem_size(zone));
		saddr = (vm_offset_t) addr - PAGE_SIZE;
	} else {
		gzh = (gzhdr_t *)((vm_offset_t)addr - GZHEADER_SIZE);
		saddr = ((vm_offset_t)addr) - residue;
	}

	if ((saddr & PAGE_MASK) != 0) {
		panic("%s: invalid address supplied: "
		    "%p (adjusted: 0x%lx) for zone with element sized 0x%lx\n",
		    __func__, addr, saddr, zone_elem_size(zone));
	}

	if (gzfc_size && gzalloc_dfree_check) {
		zone_lock(zone);
		assert(zone->z_self == zone);
		for (uint32_t gd = 0; gd < gzfc_size; gd++) {
			if (zone->gz.gzfc[gd] != saddr) {
				continue;
			}
			panic("%s: double free detected, freed address: 0x%lx, "
			    "current free cache index: %d, freed index: %d",
			    __func__, saddr, zone->gz.gzfc_index, gd);
		}
		zone_unlock(zone);
	}

	if (gzalloc_consistency_checks) {
		if (gzh->gzsig != GZALLOC_SIGNATURE) {
			panic("GZALLOC signature mismatch for element %p, "
			    "expected 0x%x, found 0x%x",
			    addr, GZALLOC_SIGNATURE, gzh->gzsig);
		}

		if (gzh->gzone != zone && (gzh->gzone != GZDEADZONE)) {
			panic("%s: Mismatched zone or under/overflow, "
			    "current zone: %p, recorded zone: %p, address: %p",
			    __func__, zone, gzh->gzone, (void *)addr);
		}
		/* Partially redundant given the zone check, but may flag header corruption */
		if (gzh->gzsize != zone_elem_size(zone)) {
			panic("Mismatched zfree or under/overflow for zone %p, "
			    "recorded size: 0x%x, element size: 0x%x, address: %p",
			    zone, gzh->gzsize, (uint32_t)zone_elem_size(zone), (void *)addr);
		}

		char *gzc, *checkstart, *checkend;
		if (gzalloc_uf_mode) {
			checkstart = (char *) ((uintptr_t) gzh + sizeof(gzh));
			checkend = (char *) ((((vm_offset_t)addr) & ~PAGE_MASK) + PAGE_SIZE);
		} else {
			checkstart = (char *) trunc_page_64(addr);
			checkend = (char *)gzh;
		}

		for (gzc = checkstart; gzc < checkend; gzc++) {
			if (*gzc == gzalloc_fill_pattern) {
				continue;
			}
			panic("%s: detected over/underflow, byte at %p, element %p, "
			    "contents 0x%x from 0x%lx byte sized zone (%s%s) "
			    "doesn't match fill pattern (%c)",
			    __func__, gzc, addr, *gzc, zone_elem_size(zone),
			    zone_heap_name(zone), zone->z_name, gzalloc_fill_pattern);
		}
	}

	if ((startup_phase < STARTUP_SUB_KMEM) || gzh->gzone == GZDEADZONE) {
		/* For now, just leak frees of early allocations
		 * performed before kmem is fully configured.
		 * They don't seem to get freed currently;
		 * consider ml_static_mfree in the future.
		 */
		OSAddAtomic64((SInt32) (rounded_size), &gzalloc_early_free);
		return;
	}

	if (get_preemption_level() != 0) {
		pdzfree_count++;
	}

	if (gzfc_size) {
		/* Either write protect or unmap the newly freed
		 * allocation
		 */
		kr = vm_map_protect(gzalloc_map, saddr,
		    saddr + rounded_size + (1 * PAGE_SIZE),
		    gzalloc_prot, FALSE);
		if (kr != KERN_SUCCESS) {
			panic("%s: vm_map_protect: %p, 0x%x", __func__, (void *)saddr, kr);
		}
	} else {
		free_addr = saddr;
	}

	zone_lock(zone);
	assert(zone->z_self == zone);

	/* Insert newly freed element into the protected free element
	 * cache, and rotate out the LRU element.
	 */
	if (gzfc_size) {
		if (zone->gz.gzfc_index >= gzfc_size) {
			zone->gz.gzfc_index = 0;
		}
		free_addr = zone->gz.gzfc[zone->gz.gzfc_index];
		zone->gz.gzfc[zone->gz.gzfc_index++] = saddr;
	}

	if (free_addr) {
		zone->z_elems_free++;
		zone->z_wired_cur -= 1;
	}

	zpercpu_get(zstats)->zs_mem_freed += rounded_size;
	zone_unlock(zone);

	if (free_addr) {
		// TODO: consider using physical reads to check for
		// corruption while on the protected freelist
		// (i.e. physical corruption)
		kmem_free(gzalloc_map, free_addr, rounded_size + PAGE_SIZE);
		// TODO: sysctl-ize for quick reference
		OSAddAtomic64((SInt32)rounded_size, &gzalloc_freed);
		OSAddAtomic64(-((SInt32) (rounded_size - zone_elem_size(zone))),
		    &gzalloc_wasted);
	}
}

boolean_t
gzalloc_element_size(void *gzaddr, zone_t *z, vm_size_t *gzsz)
{
	uintptr_t a = (uintptr_t)gzaddr;
	if (__improbable(gzalloc_mode && kmem_range_contains(&gzalloc_range, a))) {
		gzhdr_t *gzh;
		boolean_t       vmef;
		vm_map_entry_t  gzvme = NULL;
		vm_map_lock_read(gzalloc_map);
		vmef = vm_map_lookup_entry(gzalloc_map, (vm_map_offset_t)a, &gzvme);
		vm_map_unlock(gzalloc_map);
		if (vmef == FALSE) {
			panic("GZALLOC: unable to locate map entry for %p", (void *)a);
		}
		assertf(gzvme->vme_atomic != 0, "GZALLOC: VM map entry inconsistency, "
		    "vme: %p, start: %llu end: %llu", gzvme, gzvme->vme_start, gzvme->vme_end);

		/* Locate the gzalloc metadata adjoining the element */
		if (gzalloc_uf_mode == TRUE) {
			/* In underflow detection mode, locate the map entry describing
			 * the element, and then locate the copy of the gzalloc
			 * header at the trailing edge of the range.
			 */
			gzh = (gzhdr_t *)(gzvme->vme_end - GZHEADER_SIZE);
		} else {
			/* In overflow detection mode, scan forward from
			 * the base of the map entry to locate the
			 * gzalloc header.
			 */
			uint32_t *p = (uint32_t*) gzvme->vme_start;
			while (p < (uint32_t *) gzvme->vme_end) {
				if (*p == GZALLOC_SIGNATURE) {
					break;
				} else {
					p++;
				}
			}
			if (p >= (uint32_t *) gzvme->vme_end) {
				panic("GZALLOC signature missing addr %p, zone %p", gzaddr, z);
			}
			p++;
			uintptr_t q = (uintptr_t) p;
			gzh = (gzhdr_t *) (q - sizeof(gzhdr_t));
		}

		if (gzh->gzsig != GZALLOC_SIGNATURE) {
			panic("GZALLOC signature mismatch for element %p, expected 0x%x, found 0x%x",
			    (void *)a, GZALLOC_SIGNATURE, gzh->gzsig);
		}

		*gzsz = zone_elem_size(gzh->gzone);
		if (__improbable(!gzh->gzone->z_gzalloc_tracked)) {
			panic("GZALLOC: zone mismatch (%p)", gzh->gzone);
		}

		if (z) {
			*z = gzh->gzone;
		}
		return TRUE;
	} else {
		return FALSE;
	}
}
