/*
 * Portions are:
 *  Copyright (c) 2002 Pavel Machek <pavel@suse.cz>
 *  Copyright (c) 2001 Patrick Mochel <mochel@osdl.org>
 */

#include <xen/config.h>
#include <xen/acpi.h>
#include <xen/smp.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/flushtlb.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/support.h>
#include <asm/i387.h>

#if defined(CONFIG_X86_64)
static unsigned long saved_lstar, saved_cstar;
static unsigned long saved_sysenter_esp, saved_sysenter_eip;
#endif

void save_rest_processor_state(void)
{
    if ( !is_idle_vcpu(current) )
        unlazy_fpu(current);

#if defined(CONFIG_X86_64)
    rdmsrl(MSR_CSTAR, saved_cstar);
    rdmsrl(MSR_LSTAR, saved_lstar);
    if ( boot_cpu_data.x86_vendor == X86_VENDOR_INTEL )
    {
        rdmsrl(MSR_IA32_SYSENTER_ESP, saved_sysenter_esp);
        rdmsrl(MSR_IA32_SYSENTER_EIP, saved_sysenter_eip);
    }
#endif
}

void restore_rest_processor_state(void)
{
    struct vcpu *v = current;

    load_TR();

#if defined(CONFIG_X86_64)
    /* Recover syscall MSRs */
    wrmsrl(MSR_LSTAR, saved_lstar);
    wrmsrl(MSR_CSTAR, saved_cstar);
    wrmsr(MSR_STAR, 0, (FLAT_RING3_CS32<<16) | __HYPERVISOR_CS);
    wrmsr(MSR_SYSCALL_MASK,
          X86_EFLAGS_VM|X86_EFLAGS_RF|X86_EFLAGS_NT|
          X86_EFLAGS_DF|X86_EFLAGS_IF|X86_EFLAGS_TF,
          0U);

    if ( boot_cpu_data.x86_vendor == X86_VENDOR_INTEL )
    {
        /* Recover sysenter MSRs */
        wrmsrl(MSR_IA32_SYSENTER_ESP, saved_sysenter_esp);
        wrmsrl(MSR_IA32_SYSENTER_EIP, saved_sysenter_eip);
        wrmsr(MSR_IA32_SYSENTER_CS, __HYPERVISOR_CS, 0);
    }
#else /* !defined(CONFIG_X86_64) */
    if ( supervisor_mode_kernel && cpu_has_sep )
        wrmsr(MSR_IA32_SYSENTER_ESP, &this_cpu(init_tss).esp1, 0);
#endif

    /* Maybe load the debug registers. */
    BUG_ON(is_hvm_vcpu(v));
    if ( !is_idle_vcpu(v) && unlikely(v->arch.guest_context.debugreg[7]) )
    {
        write_debugreg(0, v->arch.guest_context.debugreg[0]);
        write_debugreg(1, v->arch.guest_context.debugreg[1]);
        write_debugreg(2, v->arch.guest_context.debugreg[2]);
        write_debugreg(3, v->arch.guest_context.debugreg[3]);
        write_debugreg(6, v->arch.guest_context.debugreg[6]);
        write_debugreg(7, v->arch.guest_context.debugreg[7]);
    }

    /* Reload FPU state on next FPU use. */
    stts();

    if (cpu_has_pat)
        wrmsrl(MSR_IA32_CR_PAT, host_pat);

    mtrr_bp_restore();
}
