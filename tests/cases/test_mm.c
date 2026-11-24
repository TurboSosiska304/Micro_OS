/*
 * Micro OS — the policy of a page (ADR 0052), the half a machine without page
 * tables can check.
 *
 * The test machine is a process of Linux: a program of it is a task of the
 * kernel, `k_port_mm_supported()` is false, there is no space and `mmap` is
 * ENOSYS. What can be checked here is the rule the portable kernel states
 * before any port gives a page: a page is code or it is data, and a mapping
 * that asks for both is refused. The rule lives in kernel/mm.c so that every
 * machine that ever grows a space inherits the refusal with the call - the
 * x86-64 port, and the ARM port of `docs/plan-arm64.md` when it comes.
 *
 * The other half of the file is the truth about this machine: a space of zero
 * is not a space, so a call that names one is an error and not a page table
 * that points at the kernel. The fault of a program and the growth of a stack
 * are checked on the machine they belong to (`make mmu-smoke`, ADR 0052 §7);
 * a test of them here would need an MMU this port does not have.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "kernel/errno.h"
#include "kernel/mm.h"
#include "kernel/port/mm.h"

void test_mm(void)
{
    /* --- a page is code or it is data ------------------------------- */

    KT_CHECK(k_mm_prot_allowed(0));
    KT_CHECK(k_mm_prot_allowed(K_MM_PROT_READ));
    KT_CHECK(k_mm_prot_allowed(K_MM_PROT_WRITE));
    KT_CHECK(k_mm_prot_allowed(K_MM_PROT_EXEC));
    KT_CHECK(k_mm_prot_allowed(K_MM_PROT_READ | K_MM_PROT_WRITE));
    KT_CHECK(k_mm_prot_allowed(K_MM_PROT_READ | K_MM_PROT_EXEC));

    /* The one combination the rule refuses: a page that is memory and code
     * at once. A truth table is a table, not a `!` that a second bit would
     * slip past. */
    KT_CHECK(!k_mm_prot_allowed(K_MM_PROT_WRITE | K_MM_PROT_EXEC));
    KT_CHECK(!k_mm_prot_allowed(K_MM_PROT_READ | K_MM_PROT_WRITE | K_MM_PROT_EXEC));

    /* --- this machine has no space ---------------------------------- */

    KT_CHECK(!k_port_mm_supported());
    KT_CHECK_INT_EQ((int)k_port_mm_base(), 0);
    KT_CHECK_INT_EQ((int)k_port_mm_limit(), 0);
    KT_CHECK_INT_EQ((int)k_port_mm_top(), 0);
    KT_CHECK_INT_EQ((int)k_mm_current(), 0);

    /* A mapping of no space is a mistake of the kernel and not a table that
     * points at it, and a call of the heap has nowhere to move the break. */
    {
        k_u64_t placed = 0;

        KT_CHECK_INT_EQ(k_mm_map(0, 0, K_MM_PAGE_SIZE, K_MM_PROT_READ | K_MM_PROT_WRITE,
                                 K_MM_PRIVATE | K_MM_ANONYMOUS, NULL, 0, &placed),
                        KERR_INVAL);
        KT_CHECK_INT_EQ(k_mm_brk(0, 0, &placed), KERR_NOSYS);
        KT_CHECK_INT_EQ(
            k_mm_map(0, 0, K_MM_PAGE_SIZE, K_MM_PROT_READ, K_MM_PRIVATE, NULL, 0, &placed),
            KERR_INVAL);
    }
}
