#include <sbi/riscv_encoding.h>
#include <sbi/sbi_ecall_ebi_enclave.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_trap.h>

#define MAX_PAGE 8192
static enclave_page_t pages[MAX_PAGE];
static size_t max_pages;

void poweroff(uint16_t code)
{
	sbi_printf("Power off\r\n");
	//     finisher_exit(code);
	//     if (htif) {
	//         htif_poweroff();
	//     } else {
	//         send_ipi_many(0, IPI_HALT);
	//         while (1) {
	//             asm volatile("wfi\n");
	//         }
	//     }
}

#define assert(x)                                        \
	({                                               \
		if (!(x))                                \
			die("assertion failed: %s", #x); \
	})
#define die(str, ...)                                              \
	({                                                         \
		sbi_printf("%s:%d: " str "\n", __FILE__, __LINE__, \
			   ##__VA_ARGS__);                         \
		poweroff(-1);                                      \
	})

int memset()
{
	return -1;
}

long atol(const char *str)
{
	long res = 0;
	int sign = 0;

	while (*str == ' ')
		str++;

	if (*str == '-' || *str == '+') {
		sign = *str == '-';
		str++;
	}

	while (*str) {
		res *= 10;
		res += *str++ - '0';
	}

	return sign ? -res : res;
}

static inline uint32_t load_uint32_t(const uint32_t *addr, uintptr_t mepc)
{
	register uintptr_t __mepc asm("a2") = mepc;
	register uintptr_t __mstatus asm("a3");
	uint32_t val;
	asm("csrrs %0, mstatus, %3\n"
	    "lw %1, %2\n"
	    "csrw mstatus, %0"
	    : "+&r"(__mstatus), "=&r"(val)
	    : "m"(*addr), "r"(MSTATUS_MPRV), "r"(__mepc));
	return val;
}

static inline uint8_t load_uint8_t(const uint8_t *addr, uintptr_t mepc)
{
	register uintptr_t __mepc asm("a2") = mepc;
	register uintptr_t __mstatus asm("a3");
	uint8_t val;
	asm("csrrs %0, mstatus, %3\n"
	    "lbu %1, %2\n"
	    "csrw mstatus, %0"
	    : "+&r"(__mstatus), "=&r"(val)
	    : "m"(*addr), "r"(MSTATUS_MPRV), "r"(__mepc));
	return val;
}

static inline void store_uint32_t(uint32_t *addr, uint32_t val, uintptr_t mepc)
{
	register uintptr_t __mepc asm("a2") = mepc;
	register uintptr_t __mstatus asm("a3");
	asm volatile("csrrs %0, mstatus, %3\n"
		     "sw %1, %2\n"
		     "csrw mstatus, %0"
		     : "+&r"(__mstatus)
		     : "r"(val), "m"(*addr), "r"(MSTATUS_MPRV), "r"(__mepc));
}

static inline void flush_tlb()
{
	asm volatile("sfence.vma");
}

static inline uint64_t load_uint64_t(const uint64_t *addr, uintptr_t mepc)
{
	return load_uint32_t((uint32_t *)addr, mepc) +
	       ((uint64_t)load_uint32_t((uint32_t *)addr + 1, mepc) << 32);
}

static inline void store_uint64_t(uint64_t *addr, uint64_t val, uintptr_t mepc)
{
	store_uint32_t((uint32_t *)addr, val, mepc);
	store_uint32_t((uint32_t *)addr + 1, val >> 32, mepc);
}

void enclave_mem_init()
{
	uintptr_t page_start, page_end;
	page_start		   = (uintptr_t)&_enclave_start;
	page_end		   = (uintptr_t)&_enclave_end;
	size_t enclave_memory_size = page_end - page_start;
	assert(enclave_memory_size % EPAGE_SIZE == 0);
	max_pages = enclave_memory_size / EPAGE_SIZE;

	assert(max_pages <=
	       MAX_PAGE); // ensure we can keep track of all memory.
	sbi_memset(pages, 0, sizeof(enclave_page_t) * max_pages);
	for (size_t i = 0; i < max_pages; i++) {
		pages[i].pa	= page_start + i * EPAGE_SIZE;
		pages[i].status = PAGE_FREE;
	}
}

uintptr_t enclave_mem_alloc(enclave_context *context, size_t enclave_size)
{
	enclave_page_t page;
	assert((enclave_size & MASK(EPAGE_SHIFT)) ==
	       0); // at least one page should be allocated
	size_t page_num = enclave_size >> EPAGE_SHIFT;
	/* Look for available memory */
	char ok		= 0;
	size_t page_idx = 0;
	for (size_t i = 0; i < max_pages; i++) {
		page = pages[i];
		if (page.status == PAGE_FREE) {
			ok = 1;
			for (size_t j = i + 1;
			     j - i < page_num && j < max_pages; j++) {
				page = pages[j];
				if (page.status != PAGE_FREE) {
					i  = j + 1;
					ok = 0;
					break;
				}
			}
			if (ok) {
				page_idx = i;
				break;
			}
		}
	}
	if (!ok) {
		sbi_printf("alloc enclave memory failed\n");
		return EBI_ERROR;
	}
	/* Mark used pages */
	for (size_t i = 0; i < page_num; i++) {
		pages[page_idx + i].status = PAGE_USED;
		sbi_memset((void *)pages[page_idx + i].pa, 0, EPAGE_SIZE);
	}

	page	    = pages[page_idx];
	context->pa = page.pa;
	sbi_printf("[enclave_mem_alloc] context->pa = %lx\n", context->pa);
	context->mem_size = enclave_size;
	return EBI_OK;
}

uintptr_t enclave_mem_free(enclave_context *context)
{
	assert(context->mem_size % EPAGE_SIZE == 0);
	size_t page_nums = context->mem_size >> EPAGE_SHIFT;

	uintptr_t start_page_index = (context->pa - pages[0].pa) >> EPAGE_SHIFT;
	while (page_nums-- > 0) {
		pages[start_page_index++].status = PAGE_FREE;
	}
	return EBI_OK;
}

#define NUM_ENCLAVE 2
static enclave_context enclaves[NUM_ENCLAVE + 1];

void pmp_switch(enclave_context *context)
{
	uintptr_t p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, cfg;
	if (context == NULL) {
		// Switch to Linux
		extern char _enclave_end;
		p0  = (uintptr_t)(FW_TEXT_START) >> PMP_SHIFT;
		p1  = (uintptr_t)(&_enclave_end) >> PMP_SHIFT;
		cfg = PMP_A_TOR << 8;
		p2  = 0 >> PMP_SHIFT;
		p3  = -1UL >> PMP_SHIFT;
		cfg |= (PMP_A_TOR | PMP_R | PMP_W | PMP_X) << 24;
	} else {
		// Switch to other enclave
		p0  = context->pa >> PMP_SHIFT;
		p1  = (context->pa + context->mem_size) >> PMP_SHIFT;
		cfg = (PMP_A_TOR | PMP_R | PMP_W | PMP_X) << 8;
		p2  = 0UL >> PMP_SHIFT;
		p3  = PHY_MEM_START >> PMP_SHIFT;
		cfg |= (PMP_A_TOR | PMP_R | PMP_W | PMP_X) << 24;
		p4 = PHY_MEM_END >> PMP_SHIFT;
		p5 = -1UL >> PMP_SHIFT;
		cfg |= (uintptr_t)(PMP_A_TOR | PMP_R | PMP_W | PMP_X) << 40;
	}
	asm volatile("csrw pmpaddr0, %[p0]\n\t"
		     "csrw pmpaddr1, %[p1]\n\t"
		     "csrw pmpaddr2, %[p2]\n\t"
		     "csrw pmpaddr3, %[p3]\n\t"
		     "csrw pmpaddr4, %[p4]\n\t"
		     "csrw pmpaddr5, %[p5]\n\t"
		     "csrw pmpcfg0, %[cfg]" ::[p0] "r"(p0),
		     [p1] "r"(p1), [p2] "r"(p2), [p3] "r"(p3), [p4] "r"(p4),
		     [p5] "r"(p5), [cfg] "r"(cfg));
}

void save_umode_context(enclave_context *context, struct sbi_trap_regs *regs)
{
	sbi_printf("[save_umode_context] regs @ 0x%p\n", regs);
	sbi_memcpy(context->umode_context, regs, INTEGER_CONTEXT_SIZE);
}

void restore_umode_context(enclave_context *context, struct sbi_trap_regs *regs)
{
	sbi_memcpy(regs, context->umode_context, INTEGER_CONTEXT_SIZE);
}

void save_csr_context(enclave_context *from, uintptr_t mepc,
		      struct sbi_trap_regs *regs)
{
	from->ns_satp = csr_read(CSR_SATP);

	from->ns_mepc = mepc + 4;
	// from->ns_mstatus = csr_read(CSR_MSTATUS);
	from->ns_mstatus = regs->mstatus;
	from->ns_medeleg = csr_read(CSR_MEDELEG);

	from->ns_sie	  = csr_read(CSR_SIE);
	from->ns_stvec	  = csr_read(CSR_STVEC);
	from->ns_sstatus  = csr_read(CSR_SSTATUS);
	from->ns_sscratch = csr_read(CSR_SSCRATCH);
}

void restore_csr_context(enclave_context *into, struct sbi_trap_regs *regs)
{
	csr_write(CSR_SATP, into->ns_satp);
	flush_tlb();

	// csr_write(CSR_MEPC, into->ns_mepc);
	// csr_write(CSR_MSTATUS, into->ns_mstatus);
	csr_write(CSR_MEDELEG, into->ns_medeleg);

	csr_write(CSR_SIE, into->ns_sie);
	csr_write(CSR_STVEC, into->ns_stvec);
	csr_write(CSR_SSTATUS, into->ns_sstatus);
	csr_write(CSR_SSCRATCH, into->ns_sscratch);

	regs->mepc    = into->ns_mepc - 4;
	regs->mstatus = into->ns_mstatus;
}

void init_csr_context(enclave_context *context)
{
	uintptr_t new_mstatus = csr_read(CSR_MSTATUS) &
				~(MSTATUS_MPP | MSTATUS_SIE | MSTATUS_SPIE);
	new_mstatus |= MSTATUS_MPP & (MSTATUS_MPP >> 1);
	new_mstatus |= MSTATUS_SIE;
	context->ns_mstatus = new_mstatus;

	context->ns_sstatus = csr_read(CSR_SSTATUS) & ~(SSTATUS_SIE);
	context->ns_mepc    = 0x0 + context->pa + EUSR_MEM_SIZE;
	// context->ns_medeleg = (csr_read(CSR_MEDELEG) | (1U << CAUSE_FETCH_ACCESS) | (1U << CAUSE_USER_ECALL)) & ~(1U << CAUSE_SUPERVISOR_ECALL);
	context->ns_sscratch = 0;
	context->ns_satp     = 0;
	context->ns_sie	     = 0;
	context->ns_stvec    = 0;
}

/* Machine mode does not contains the necessary mapping, need to switch back to
 * user mode */
void memcpy_from_user(uintptr_t uaddr, uintptr_t maddr, uintptr_t size,
		      uintptr_t mepc)
{
	while (size > 0) {
		/* 8 bytes per iter */
		if (size >= 8) {
			*(uint64_t *)maddr =
				load_uint64_t((uint64_t *)uaddr, mepc);
			maddr += 8;
			uaddr += 8;
			size -= 8;
		} else {
			*(uint8_t *)maddr =
				load_uint8_t((uint8_t *)uaddr, mepc);
			sbi_printf("%c\n", *(char *)maddr);
			++maddr;
			++uaddr;
			size -= 1;
		}
	}
}

uintptr_t find_avail_enclave()
{
	for (size_t i = 0; i < NUM_ENCLAVE; ++i) {
		if (enclaves[i].status == ENC_FREE)
			return i;
	}
	return EBI_ERROR;
}

void init_enclaves(void)
{
	enclave_mem_init();
	sbi_memset(enclaves, 0, sizeof(enclave_context) * NUM_ENCLAVE);
	enclaves[NUM_ENCLAVE].status = ENC_RUN;
	for (size_t i = 0; i < NUM_ENCLAVE; ++i)
		enclaves[i].status = ENC_FREE;
	sbi_printf("[EBI] enclaves init successfully!");
}

uintptr_t create_enclave(const struct sbi_trap_regs *regs, uintptr_t mepc)
{
	uintptr_t payload_addr;
	payload_addr		 = regs->a0;
	size_t payload_size	 = regs->a1;
	uintptr_t driver_bitmask = regs->a2;

	sbi_printf("[create_enclave] user_payload_addr = 0x%lx\n",
		   payload_addr);

	if (PAGE_UP(payload_size) > EMEM_SIZE)
		return EBI_ERROR;

	sbi_printf("[create_enclave] log1\n");

	enclave_context *context = NULL;
	uintptr_t avail_id	 = find_avail_enclave();

	if (avail_id == EBI_ERROR)
		return EBI_ERROR;
	/* LOADED enclave, loaded not running */
	enclaves[avail_id].status = ENC_LOAD;
	context			  = &enclaves[avail_id];
	sbi_printf("[create_enclave] log2\n");

	if (EBI_OK != enclave_mem_alloc(context, EMEM_SIZE))
		return EBI_ERROR;
	sbi_printf("[create_enclave] log3\n");

	uintptr_t base_module_copy_start, base_module_copy_end;
	base_module_copy_start = (uintptr_t)&_base_start;
	base_module_copy_end   = (uintptr_t)&_base_end;
	size_t base_module_size =
		PAGE_UP(base_module_copy_end - base_module_copy_start);
	uintptr_t base_module_start = context->pa + EUSR_MEM_SIZE;
	uintptr_t extra_module_size = 0;

	// base module copying
	sbi_printf(
		"[create_enclave] copying base module: from 0x%lx copy to 0x%lx\n",
		base_module_copy_start, base_module_start);
	sbi_memcpy((void *)base_module_start, (void *)base_module_copy_start,
		   base_module_size);

	// extra modules copying according to the module list
	sbi_printf("[create_enclave] copying extra modules: bitmask: 0x%lx\n",
		   driver_bitmask);
	base_module_start += base_module_size;
	if (driver_bitmask != 0)
		extra_module_size = drvcpy(&base_module_start, driver_bitmask);
	else
		base_module_start = 0;
	sbi_printf("[create_enclave] enclave pa = 0x%lx\n", context->pa);
	memcpy_from_user(payload_addr, context->pa, payload_size, mepc);
	init_csr_context(context);

	context->enclave_binary_size = payload_size;
	context->drv_list	     = base_module_start;
	context->user_param	     = base_module_start + extra_module_size;
	// args[0]			     = avail_id;
	return avail_id;
}

// regs changed to args
uintptr_t enter_enclave(struct sbi_trap_regs *regs, uintptr_t mepc)
{
	uintptr_t id	      = regs->a0;
	enclave_context *into = &enclaves[id], *from = &enclaves[NUM_ENCLAVE];

	sbi_printf("[enter_enclave] log1\n");
	if (into->status != ENC_LOAD || from->status != ENC_RUN)
		return EBI_ERROR;

	sbi_printf("[enter_enclave] log2\n");
	memcpy_from_user(regs->a2, into->user_param, regs->a1, mepc);

	sbi_printf("[enter_enclave] log3\n");
	pmp_switch(into);
	save_umode_context(from, regs); // this line is not compatible !!!
	save_csr_context(from, mepc, regs);
	restore_csr_context(into, regs);
	flush_tlb();

	/* User parameter */
	/* argc and argv */
	regs->a4 = regs->a0;
	regs->a5 = into->user_param;

	sbi_printf("[enter_enclave] into->pa = 0x%lx\n", into->pa);

	regs->a0     = id;
	regs->a1     = into->pa;
	regs->a2     = into->enclave_binary_size;
	regs->a3     = into->drv_list;
	into->status = ENC_RUN;
	from->status = ENC_IDLE;
	return regs->a0;
}

uintptr_t exit_enclave(struct sbi_trap_regs *regs)
{
	uintptr_t id = regs->a0, retval = regs->a1;

	enclave_context *from = &(enclaves[id]), *into = &enclaves[NUM_ENCLAVE];
	if (from->status != ENC_RUN || into->status != ENC_IDLE)
		return EBI_ERROR;

	sbi_memset((void *)from->pa, 0, EMEM_SIZE);
	enclave_mem_free(from);
	// clean and switch pmp
	pmp_switch(NULL);
	restore_umode_context(into, regs);
	restore_csr_context(into, regs);

	regs->a0 = retval;

	from->status = ENC_FREE;
	into->status = ENC_RUN;
	return EBI_OK;
}
// TODO: actually pause/resume can replace enter/exit
/* pause an enclave, do not take care of ret */
uintptr_t pause_enclave(uintptr_t id, uintptr_t *regs, uintptr_t mepc)
{
	// enclave_context *from = &(enclaves[id]);
	// // ensure one can only pause itself
	// if (from->status != ENC_RUN)
	// 	return EBI_ERROR;

	// save_umode_context(from, regs);
	// save_csr_context(from, mepc);
	// // protect entire enclave section
	// pmp_switch(NULL);
	// /* Stop paging and save page table */
	// csr_write(CSR_SATP, 0x0);
	// flush_tlb();

	// from->status = ENC_IDLE;
	return 0;
}
/* resume certain enclave, must exist */
uintptr_t resume_enclave(uintptr_t id, uintptr_t *regs)
{
	// enclave_context *into = &(enclaves[id]);
	// if (into->status != ENC_IDLE && into->status != ENC_LOAD)
	// 	return EBI_ERROR;

	// pmp_switch(into);
	// restore_csr_context(into);
	// if (into->status == ENC_IDLE)
	// 	restore_umode_context(into, regs);
	// else {
	// 	/* User parameter */
	// 	regs[A4_INDEX] = regs[A1_INDEX];
	// 	regs[A5_INDEX] = regs[A2_INDEX];

	// 	regs[A0_INDEX] = id;
	// 	regs[A1_INDEX] = into->pa;
	// 	regs[A2_INDEX] = into->enclave_binary_size;
	// 	regs[A3_INDEX] = into->drv_list;
	// }
	// into->status = ENC_RUN;
	// return id;
	return 0;
}

extern char _console_start, _console_end;
// extern char _drv_rtc_start, _drv_rtc_end;
drv_addr_t bbl_addr_list[MAX_DRV] = {
	{ (uintptr_t)&_console_start, (uintptr_t)&_console_end, -1 }
	//     {(uintptr_t)&_drv_rtc_start, (uintptr_t)&_drv_rtc_end, -1}
};

// drv_addr_t bbl_addr_list[MAX_DRV] = {};

uintptr_t drvcpy(uintptr_t *start_addr, uintptr_t bitmask)
{
	drv_addr_t drv_addr_list[64] = {};
	int cnt			     = 0;
	for (int i = 0; i < MAX_DRV; i++) {
		if (bbl_addr_list[i].drv_start && (bitmask & (1 << i))) {
			sbi_printf("[drvcpy] cnt = %d\n", cnt);
			uintptr_t drv_start = bbl_addr_list[i].drv_start;
			uintptr_t drv_size  = bbl_addr_list[i].drv_end -
					     bbl_addr_list[i].drv_start;
			drv_addr_list[cnt].drv_start = *start_addr;
			drv_addr_list[cnt].drv_end   = *start_addr + drv_size;
			sbi_printf(
				"[drvcpy] drv %d: start = 0x%lx end = 0x%lx\n",
				i, drv_addr_list[cnt].drv_start,
				drv_addr_list[cnt].drv_end);
			cnt++;
			sbi_memcpy((void *)(*start_addr), (void *)drv_start,
				   drv_size);
			*start_addr += drv_size;
		}
	}
	sbi_memcpy((void *)*start_addr, (void *)drv_addr_list,
		   sizeof(drv_addr_list));
	return sizeof(drv_addr_list);
}

char drvfetch(int enclave_id, int driver_id)
{
	if (driver_id >= MAX_DRV)
		return -1;
	if (atomic_read((atomic_t *)&bbl_addr_list[driver_id].using_by) ==
	    enclave_id)
		return enclave_id;
	while (atomic_cas(&bbl_addr_list[driver_id].using_by, -1, enclave_id) !=
	       enclave_id)
		;
	return enclave_id;
}

void drvrelease(int enclave_id, int driver_id)
{
	if (driver_id >= MAX_DRV) {
		return;
	}
	atomic_cas(&bbl_addr_list[driver_id].using_by, enclave_id, -1);
}