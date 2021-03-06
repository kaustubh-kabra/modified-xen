#include <xen/config.h>
#include <xen/init.h>
#include <xen/kernel.h>
#include <xen/string.h>
#include <xen/bitops.h>
#include <xen/smp.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/uaccess.h>
#include <asm/mpspec.h>
#include <asm/apic.h>
#include <asm/i387.h>
#include <mach_apic.h>
#include <asm/hvm/support.h>
#include <asm/setup.h>

#include "cpu.h"

#define select_idle_routine(x) ((void)0)

extern int trap_init_f00f_bug(void);

/*
 * opt_cpuid_mask_ecx/edx: cpuid.1[ecx, edx] feature mask.
 * For example, E8400[Intel Core 2 Duo Processor series] ecx = 0x0008E3FD, 
 * edx = 0xBFEBFBFF when executing CPUID.EAX = 1 normally. If you want to
 * 'rev down' to E8400, you can set these values in these Xen boot parameters.
 */
static unsigned int opt_cpuid_mask_ecx, opt_cpuid_mask_edx;
integer_param("cpuid_mask_ecx", opt_cpuid_mask_ecx);
integer_param("cpuid_mask_edx", opt_cpuid_mask_edx);

static int use_xsave = 1;
boolean_param("xsave", use_xsave);

#ifdef CONFIG_X86_INTEL_USERCOPY
/*
 * Alignment at which movsl is preferred for bulk memory copies.
 */
struct movsl_mask movsl_mask __read_mostly;
#endif

static void __devinit set_cpuidmask(void)
{
	unsigned int eax, ebx, ecx, edx, model;

	if (!(opt_cpuid_mask_ecx | opt_cpuid_mask_edx))
		return;

	cpuid(0x00000001, &eax, &ebx, &ecx, &edx);
	model = ((eax & 0xf0000) >> 12) | ((eax & 0xf0) >> 4);
	if (!((model == 0x1d) || ((model == 0x17) && ((eax & 0xf) >= 4)))) {
		printk(XENLOG_ERR "Cannot set CPU feature mask on CPU#%d\n",
		       smp_processor_id());
		return;
	}

	wrmsr(MSR_IA32_CPUID_FEATURE_MASK1,
	      opt_cpuid_mask_ecx ? : ~0u,
	      opt_cpuid_mask_edx ? : ~0u);
}

void __devinit early_intel_workaround(struct cpuinfo_x86 *c)
{
	if (c->x86_vendor != X86_VENDOR_INTEL)
		return;
	/* Netburst reports 64 bytes clflush size, but does IO in 128 bytes */
	if (c->x86 == 15 && c->x86_cache_alignment == 64)
		c->x86_cache_alignment = 128;
}

/*
 * P4 Xeon errata 037 workaround.
 * Hardware prefetcher may cause stale data to be loaded into the cache.
 */
static void __devinit Intel_errata_workarounds(struct cpuinfo_x86 *c)
{
	unsigned long lo, hi;

	if ((c->x86 == 15) && (c->x86_model == 1) && (c->x86_mask == 1)) {
		rdmsr (MSR_IA32_MISC_ENABLE, lo, hi);
		if ((lo & (1<<9)) == 0) {
			printk (KERN_INFO "CPU: C0 stepping P4 Xeon detected.\n");
			printk (KERN_INFO "CPU: Disabling hardware prefetching (Errata 037)\n");
			lo |= (1<<9);	/* Disable hw prefetching */
			wrmsr (MSR_IA32_MISC_ENABLE, lo, hi);
		}
	}
}


/*
 * find out the number of processor cores on the die
 */
static int __devinit num_cpu_cores(struct cpuinfo_x86 *c)
{
	unsigned int eax, ebx, ecx, edx;

	if (c->cpuid_level < 4)
		return 1;

	/* Intel has a non-standard dependency on %ecx for this CPUID level. */
	cpuid_count(4, 0, &eax, &ebx, &ecx, &edx);
	if (eax & 0x1f)
		return ((eax >> 26) + 1);
	else
		return 1;
}

static void __devinit init_intel(struct cpuinfo_x86 *c)
{
	unsigned int l2 = 0;
	char *p = NULL;

#ifdef CONFIG_X86_F00F_BUG
	/*
	 * All current models of Pentium and Pentium with MMX technology CPUs
	 * have the F0 0F bug, which lets nonprivileged users lock up the system.
	 * Note that the workaround only should be initialized once...
	 */
	c->f00f_bug = 0;
	if ( c->x86 == 5 ) {
		static int f00f_workaround_enabled = 0;

		c->f00f_bug = 1;
		if ( !f00f_workaround_enabled ) {
			trap_init_f00f_bug();
			printk(KERN_NOTICE "Intel Pentium with F0 0F bug - workaround enabled.\n");
			f00f_workaround_enabled = 1;
		}
	}
#endif

	select_idle_routine(c);
	l2 = init_intel_cacheinfo(c);
	if (c->cpuid_level > 9) {
		unsigned eax = cpuid_eax(10);
		/* Check for version and the number of counters */
		if ((eax & 0xff) && (((eax>>8) & 0xff) > 1))
			set_bit(X86_FEATURE_ARCH_PERFMON, c->x86_capability);
	}

	/* SEP CPUID bug: Pentium Pro reports SEP but doesn't have it until model 3 mask 3 */
	if ((c->x86<<8 | c->x86_model<<4 | c->x86_mask) < 0x633)
		clear_bit(X86_FEATURE_SEP, c->x86_capability);

	/* Names for the Pentium II/Celeron processors 
	   detectable only by also checking the cache size.
	   Dixon is NOT a Celeron. */
	if (c->x86 == 6) {
		switch (c->x86_model) {
		case 5:
			if (c->x86_mask == 0) {
				if (l2 == 0)
					p = "Celeron (Covington)";
				else if (l2 == 256)
					p = "Mobile Pentium II (Dixon)";
			}
			break;
			
		case 6:
			if (l2 == 128)
				p = "Celeron (Mendocino)";
			else if (c->x86_mask == 0 || c->x86_mask == 5)
				p = "Celeron-A";
			break;
			
		case 8:
			if (l2 == 128)
				p = "Celeron (Coppermine)";
			break;
		}
	}

	if ( p )
		safe_strcpy(c->x86_model_id, p);
	
	c->x86_max_cores = num_cpu_cores(c);

	detect_ht(c);

	set_cpuidmask();

	/* Work around errata */
	Intel_errata_workarounds(c);

#ifdef CONFIG_X86_INTEL_USERCOPY
	/*
	 * Set up the preferred alignment for movsl bulk memory moves
	 */
	switch (c->x86) {
	case 4:		/* 486: untested */
		break;
	case 5:		/* Old Pentia: untested */
		break;
	case 6:		/* PII/PIII only like movsl with 8-byte alignment */
		movsl_mask.mask = 7;
		break;
	case 15:	/* P4 is OK down to 8-byte alignment */
		movsl_mask.mask = 7;
		break;
	}
#endif

	if (c->x86 == 15)
		set_bit(X86_FEATURE_P4, c->x86_capability);
	if (c->x86 == 6) 
		set_bit(X86_FEATURE_P3, c->x86_capability);
	if ((c->x86 == 0xf && c->x86_model >= 0x03) ||
		(c->x86 == 0x6 && c->x86_model >= 0x0e))
		set_bit(X86_FEATURE_CONSTANT_TSC, c->x86_capability);
	if (cpuid_edx(0x80000007) & (1u<<8)) {
		set_bit(X86_FEATURE_CONSTANT_TSC, c->x86_capability);
		set_bit(X86_FEATURE_NONSTOP_TSC, c->x86_capability);
		set_bit(X86_FEATURE_TSC_RELIABLE, c->x86_capability);
	}
	if ((c->cpuid_level >= 0x00000006) &&
	    (cpuid_eax(0x00000006) & (1u<<2)))
		set_bit(X86_FEATURE_ARAT, c->x86_capability);

	start_vmx();

	if ( !use_xsave )
		clear_bit(X86_FEATURE_XSAVE, boot_cpu_data.x86_capability);

	if ( cpu_has_xsave )
		xsave_init();
}


static unsigned int intel_size_cache(struct cpuinfo_x86 * c, unsigned int size)
{
	/* Intel PIII Tualatin. This comes in two flavours.
	 * One has 256kb of cache, the other 512. We have no way
	 * to determine which, so we use a boottime override
	 * for the 512kb model, and assume 256 otherwise.
	 */
	if ((c->x86 == 6) && (c->x86_model == 11) && (size == 0))
		size = 256;
	return size;
}

static struct cpu_dev intel_cpu_dev __devinitdata = {
	.c_vendor	= "Intel",
	.c_ident 	= { "GenuineIntel" },
	.c_models = {
		{ .vendor = X86_VENDOR_INTEL, .family = 4, .model_names = 
		  { 
			  [0] = "486 DX-25/33", 
			  [1] = "486 DX-50", 
			  [2] = "486 SX", 
			  [3] = "486 DX/2", 
			  [4] = "486 SL", 
			  [5] = "486 SX/2", 
			  [7] = "486 DX/2-WB", 
			  [8] = "486 DX/4", 
			  [9] = "486 DX/4-WB"
		  }
		},
		{ .vendor = X86_VENDOR_INTEL, .family = 5, .model_names =
		  { 
			  [0] = "Pentium 60/66 A-step", 
			  [1] = "Pentium 60/66", 
			  [2] = "Pentium 75 - 200",
			  [3] = "OverDrive PODP5V83", 
			  [4] = "Pentium MMX",
			  [7] = "Mobile Pentium 75 - 200", 
			  [8] = "Mobile Pentium MMX"
		  }
		},
		{ .vendor = X86_VENDOR_INTEL, .family = 6, .model_names =
		  { 
			  [0] = "Pentium Pro A-step",
			  [1] = "Pentium Pro", 
			  [3] = "Pentium II (Klamath)", 
			  [4] = "Pentium II (Deschutes)", 
			  [5] = "Pentium II (Deschutes)", 
			  [6] = "Mobile Pentium II",
			  [7] = "Pentium III (Katmai)", 
			  [8] = "Pentium III (Coppermine)", 
			  [10] = "Pentium III (Cascades)",
			  [11] = "Pentium III (Tualatin)",
		  }
		},
		{ .vendor = X86_VENDOR_INTEL, .family = 15, .model_names =
		  {
			  [0] = "Pentium 4 (Unknown)",
			  [1] = "Pentium 4 (Willamette)",
			  [2] = "Pentium 4 (Northwood)",
			  [4] = "Pentium 4 (Foster)",
			  [5] = "Pentium 4 (Foster)",
		  }
		},
	},
	.c_init		= init_intel,
	.c_identify	= generic_identify,
	.c_size_cache	= intel_size_cache,
};

int __init intel_cpu_init(void)
{
	cpu_devs[X86_VENDOR_INTEL] = &intel_cpu_dev;
	return 0;
}

// arch_initcall(intel_cpu_init);

