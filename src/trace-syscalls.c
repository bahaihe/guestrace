#include <capstone/capstone.h>
#include <libvmi/libvmi.h>
#include <libvmi/events.h>
#include <libvmi/libvmi_extra.h>
#include <glib.h>
#include <libxl_utils.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "guestrace.h"
#include "guestrace-private.h"
#include "functions-linux.h"
#include "functions-windows.h"
#include "trace-syscalls.h"

/*
 * High-level design:
 *
 * This code relies on Xen's interface to Second Level Address Translation,
 * or SLAT. See:
 *
 * 	https://blog.xenproject.org/2016/04/13/stealthy-monitoring-with-xen-altp2m/
 *
 * guestrace maintains two page tables: The first page table (PT_1) maps the
 * kernel with no modifications. The second (PT_n/the shadow page table) adds
 * breakpoints to the kernel.
 *
 * Guestrace switches between these two page tables under the following
 * conditions:
 *
 * Guestrace activates PT_1:
 *
 * 	(1) For a single instruction after trapping a read; on Windows, such
 * 	a read is likely the result of Kernel Patch Protection. This allows
 * 	KPP to measure the expected kernel.
 *
 * 	(2) For a single instruction after trapping a guestrace-emplaced
 * 	breakpoint. This allows the kernel to execute as expected after
 * 	servicing the breakpoint.
 *
 * Guestrace activates PT_n following a single-step execution. This restores
 * guestrace's breakpoints after condition (1) or (2) above.
 *
 *
 *
 * Guestrace makes use of two types of breakpoints:
 *
 * Guestrace places a type-one breakpoint as the first instruction in each
 * per-system-call implementation routine. For example, Linux's sys_open,
 * which is called by the handler pointed to by LSTAR. Each type-one
 * breakpoint exists as a modification of some shadow page. Guestrace can easily
 * identify the addresses which require type-one breakpoints; it merely looks
 * up the function name using libvmi. (We do not break on the function pointed
 * to by LSTAR because a program might want to break only on select system
 * calls for performance reasons.
 *
 * Guestrace places a type-two breakpoints on the return path between each
 * per-system-call implementation routine and its caller. To avoid disassembly,
 * guestrace uses a trampoline. To avoid allocating new page in the guest
 * operating system's memory space, guestrace makes use of an existing break-
 * point instruction byte in the guest operating system's memory space. At
 * start-up time, guestrace finds the instruction. While servicing each
 * type-one breakpoint, guestrace overwrites the stack so that the return
 * executes the breakpoint instruction. After servicing a type-two breakpoing,
 * guestrace sets RIP to the original return point.
 */

/* Number of bits available for page offset. */
#define GT_PAGE_OFFSET_BITS 12

/* Default page size on our domain. */
#define GT_PAGE_SIZE (1 << GT_PAGE_OFFSET_BITS)

/* Intel breakpoint interrupt (INT 3) instruction. */
static uint8_t GT_BREAKPOINT_INST = 0xCC;

/*
 * Handle terminating signals by setting interrupted flag. This allows
 * a graceful exit.
 */
static gboolean gt_interrupted = FALSE;

/*
 * A record which describes a frame. The children of these records (themselves
 * of type gt_paddr_record) describe the physical addresses contained in the
 * frame which themselves contain a breakpoint. This is needed when the guest
 * operating system triggers such a breakpoint.
 *
 * Stored in gt_page_record_collection.
 * 	Key:   addr_t (shadow frame)
 * 	Value: gt_page_record
 */
typedef struct gt_page_record {
	addr_t      frame;
	addr_t      shadow_frame;
	GHashTable *children;
	GTLoop     *loop;
} gt_page_record;

/*
 * A record which describes the information associated with a physical address
 * which contains a breakpoint.
 *
 * Stored in children field of gt_page_record.
 * 	Key:   addr_t (shadow offset)
 * 	Value: gt_paddr_record
 */
typedef struct gt_paddr_record {
	addr_t          offset;
	GTSyscallFunc   syscall_cb;
	GTSysretFunc    sysret_cb;
	gt_page_record *parent;
	void           *data; /* Optional; passed to syscall_cb. */
} gt_paddr_record;

/*
 * Describes the state associated with a system call. This information is
 * stored and later made available while servicing the corresponding
 * system return.
 *
 * Stored in gt_ret_addr_mapping.
 * 	Key:   addr_t (thread_id AKA thread's stack pointer)
 * 	Value: gt_syscall_state
 */
typedef struct gt_syscall_state {
	gt_paddr_record *syscall_paddr_record;
	void            *data;
	addr_t           thread_id; /* needed for teardown */
} gt_syscall_state;

/*
 * Restore a stack return pointer; useful to ensure the kernel continues to
 * run after guestrace exit. Otherwise, guestrace's stack manipulation might
 * remain in place, since guestrace might no longer exist at the time of a
 * system-call return.
 */
static void
gt_restore_return_addr (gpointer data)
{
	status_t status;
	gt_syscall_state *sys_state = data;
	GTLoop *loop = sys_state->syscall_paddr_record->parent->loop;

	addr_t pa = vmi_translate_kv2p(loop->vmi, sys_state->thread_id);
	if (0 == pa) {
		fprintf(stderr, "error restoring stack; guest will likely fail\n");
		goto done;
	}

	status = vmi_write_64_pa(loop->vmi, pa, &loop->return_addr);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "error restoring stack; guest will likely fail\n");
		goto done;
	}

done:
	g_free(sys_state);
}

static void
gt_destroy_page_record (gpointer data) {
	gt_page_record *page_record = data;

	g_hash_table_destroy(page_record->children);

	/* Stop monitoring this page. */
	vmi_set_mem_event(page_record->loop->vmi,
	                  page_record->frame,
	                  VMI_MEMACCESS_N,
	                  page_record->loop->shadow_view);

	xc_altp2m_change_gfn(page_record->loop->xch,
	                     page_record->loop->domid,
	                     page_record->loop->shadow_view,
	                     page_record->shadow_frame,
	                    ~0);

	xc_domain_decrease_reservation_exact(page_record->loop->xch,
	                                     page_record->loop->domid,
	                                     1, 0,
	                                    &page_record->shadow_frame);

	g_free(page_record);
}

/*
 * Callback after a step event on any VCPU.
 */
static event_response_t
gt_singlestep_cb(vmi_instance_t vmi, vmi_event_t *event) {
	/* Resume use of shadow SLAT. */
	GTLoop *loop = event->data;
	event->slat_id = loop->shadow_view;

	/* Turn off single-step and switch slat_id. */
	return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP
	     | VMI_EVENT_RESPONSE_VMM_PAGETABLE_ID;
}

/*
 * Preemptively create the step events needed for each VCPU so we don't have to
 * create a new event each time we want to single-step.
 */
static bool
gt_set_up_step_events (GTLoop *loop)
{
	bool ok = false;

	int vcpus = vmi_get_num_vcpus(loop->vmi);
	if (0 == vcpus) {
		fprintf(stderr, "failed to get number of VCPUs\n");
		goto done;
	}

	if (vcpus > _GT_MAX_VCPUS) {
		fprintf(stderr, "guest has more VCPUs than supported\n");
		goto done;
	}

	for (int vcpu = 0; vcpu < vcpus; vcpu++) {
		SETUP_SINGLESTEP_EVENT(&(loop->step_event[vcpu]),
		                       1u << vcpu,
		                       gt_singlestep_cb,
		                       0);
		loop->step_event[vcpu].data = loop;

		if (VMI_SUCCESS != vmi_register_event(loop->vmi,
		                                     &loop->step_event[vcpu])) {
			fprintf(stderr,
			       "register single-step event on VCPU failed %d\n",
			        vcpu);
			goto done;
		}
	}

	ok = true;

done:
	return ok;
}

/*
 * Return the paddr_record associated with the given physical address.
 *
 * First obtain the page record associated with the physical address's
 * page, and then obtain the child within that record which is associated
 * with the physical address. Recall that a given page might contain
 * multiple breakpoints.
 */
static gt_paddr_record *
gt_paddr_record_from_pa(GTLoop *loop, addr_t pa) {
	gt_paddr_record *paddr_record = NULL;
	gt_page_record  *page_record  = NULL;

	addr_t frame  = pa >> GT_PAGE_OFFSET_BITS;
	addr_t offset = pa  % GT_PAGE_SIZE;
	addr_t shadow = (addr_t)g_hash_table_lookup(loop->gt_page_translation,
	                                            GSIZE_TO_POINTER(frame));
	if (0 == shadow) {
		goto done;
	}

	page_record = g_hash_table_lookup(loop->gt_page_record_collection,
	                                  GSIZE_TO_POINTER(shadow));
	if (NULL == page_record) {
		goto done;
	}

	paddr_record = g_hash_table_lookup(page_record->children,
	                                   GSIZE_TO_POINTER(offset));

done:
	return paddr_record;
}

/* Return the paddr_record associated with the given virtual address. */
static gt_paddr_record *
gt_paddr_record_from_va(GTLoop *loop, addr_t va) {
	return gt_paddr_record_from_pa(loop, vmi_translate_kv2p(loop->vmi, va));
}

/*
 * Service a triggered breakpoint. Restore the original page table for one
 * single-step iteration and invoke the system call or return callback.
 *
 * In the case of a system call, enable the syscall return breakpoint.
 *
 * In the case of a system return, disable the syscall return breakpoint until
 * the next system call enables it.
 */
static event_response_t
gt_breakpoint_cb(vmi_instance_t vmi, vmi_event_t *event) {
	event_response_t response = VMI_EVENT_RESPONSE_NONE;

	GTLoop *loop = event->data;
	event->interrupt_event.reinject = 0;

	if (event->interrupt_event.gla != loop->trampoline_addr) {
		/* Type-one breakpoint (system call). */
		status_t status;
		gt_paddr_record *record;
		gt_syscall_state *state;

		/* Lookup record corresponding to breakpoint address. */
		record = gt_paddr_record_from_va(loop,
		                                 event->interrupt_event.gla);
		if (NULL == record) {
			/* Assume we didn't emplace interrupt. */
			event->interrupt_event.reinject = 1;
			/* TODO: Ensure this does the right thing: */
			goto done;
		}

		/* Set VCPUs SLAT to use original for one step. */
		event->slat_id = 0;

		/* Turn on single-step and switch slat_id after return. */
		response = VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP
		         | VMI_EVENT_RESPONSE_VMM_PAGETABLE_ID;

		addr_t thread_id  = event->x86_regs->rsp;
		addr_t return_loc = vmi_translate_kv2p(vmi, thread_id);
		if (0 == return_loc) {
			goto done;
		}

		addr_t return_addr = 0;
		status = vmi_read_64_pa(vmi, return_loc, &return_addr);
		if (VMI_SUCCESS != status || return_addr != loop->return_addr) {
			/* Return pointer not as expected. */
			goto done;
		}

		vmi_pid_t pid = vmi_dtb_to_pid(vmi, event->x86_regs->cr3);

		state                       = g_new0(gt_syscall_state, 1);
		state->syscall_paddr_record = record;
		state->thread_id            = thread_id;

		/* Invoke system-call callback in record. */
		state->data          = record->syscall_cb(vmi, event, pid,
		                                          thread_id,
		                                          record->data);

		/* Record system-call state. */
		g_hash_table_insert(loop->gt_ret_addr_mapping,
		                    GSIZE_TO_POINTER(thread_id),
		                    state);

		/* Overwrite stack to return to trampoline. */
		vmi_write_64_pa(vmi, return_loc, &loop->trampoline_addr);
	} else {
		/* Type-two breakpoint (system return). */
		gt_syscall_state *state;
		addr_t thread_id = event->x86_regs->rsp - loop->return_addr_width;

		state = g_hash_table_lookup(loop->gt_ret_addr_mapping,
		                            GSIZE_TO_POINTER(thread_id));

		if (NULL != state) {
			vmi_pid_t pid = vmi_dtb_to_pid(vmi, event->x86_regs->cr3);

			state->syscall_paddr_record->sysret_cb(vmi,
			                                       event,
			                                       pid,
			                                       thread_id,
			                                       state->data);

			vmi_set_vcpureg(vmi, loop->return_addr, RIP, event->vcpu_id);

			/*
			 * This will free our gt_syscall_state object, but
			 * sysret_cb must have freed state->data.
			 */
			g_hash_table_remove(loop->gt_ret_addr_mapping,
			                    GSIZE_TO_POINTER(thread_id));
		}
	}

done:
	return response;
}

/*
 * Callback invoked on a R/W of a monitored page (likely Windows kernel patch
 * protection). Switch the VCPU's SLAT to its original, step once, switch SLAT
 * back.
 */
static event_response_t
gt_mem_rw_cb (vmi_instance_t vmi, vmi_event_t *event) {
	/* Switch back to original SLAT for one step. */
	event->slat_id = 0;

	return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP
	     | VMI_EVENT_RESPONSE_VMM_PAGETABLE_ID;
}

/*
 * Setup our global interrupt to catch any interrupts on any pages.
 */
static bool
gt_set_up_generic_events (GTLoop *loop) {
	bool ok = false;
	status_t status;

	SETUP_INTERRUPT_EVENT(&loop->breakpoint_event, 0, gt_breakpoint_cb);
	loop->breakpoint_event.data = loop;

	status = vmi_register_event(loop->vmi, &loop->breakpoint_event);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "Failed to setup interrupt event\n");
		goto done;
	}

	SETUP_MEM_EVENT(&loop->memory_event,
	                ~0ULL,
	                 VMI_MEMACCESS_RW,
	                 gt_mem_rw_cb,
	                 1);

	loop->memory_event.data = loop;

	status = vmi_register_event(loop->vmi, &loop->memory_event);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "failed to setup memory event\n");
		goto done;
	}

	ok = true;

done:
	return ok;
}

/* Search the kernel code address space for an existing int 3 instruction. */
addr_t
gt_find_trampoline_addr (GTLoop *loop)
{
	addr_t trampoline_addr = 0;
	status_t status;
	addr_t lstar = 0;
	uint8_t code[GT_PAGE_SIZE] = { 0 }; /* Assume CALL is within first page. */

	/* LSTAR should be constant across all VCPUs */
	status = vmi_get_vcpureg(loop->vmi, &lstar, MSR_LSTAR, 0);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "failed to get MSR_LSTAR address\n");
		goto done;
	}

	addr_t lstar_p = vmi_translate_kv2p(loop->vmi, lstar);
	if (0 == lstar_p) {
		fprintf(stderr, "failed to translate virtual LSTAR to physical address");
		goto done;
	}

	/* Read kernel instructions into code. */
	status = vmi_read_pa(loop->vmi, lstar_p, code, sizeof(code));
	if (status < GT_PAGE_SIZE) {
		fprintf(stderr, "failed to read instructions from 0x%lx.\n",
		                 lstar_p);
		goto done;
	}

	/* Look for int 3. */
	for (int curr_inst = 0; curr_inst < GT_PAGE_SIZE; curr_inst++) {
		if (code[curr_inst] == GT_BREAKPOINT_INST) {
			trampoline_addr = lstar + curr_inst;
			break;
		}
	}

done:
	return trampoline_addr;
}

/**
 * gt_loop_new:
 * @guest_name: the name of a running guest virtual machine.
 *
 * Creates a new #GTLoop structure.
 *
 * Returns: a new #GTLoop.
 **/
GTLoop *gt_loop_new(const char *guest_name)
{
	GTLoop *loop;
	int rc;
	status_t status = VMI_FAILURE;

	loop = g_new0(GTLoop, 1);

	/* Initialize the libvmi library. */
	status = vmi_init(&loop->vmi,
	                   VMI_XEN | VMI_INIT_COMPLETE | VMI_INIT_EVENTS,
	                   guest_name);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "failed to init LibVMI library.\n");
		goto done;
	} else {
		printf("LibVMI init succeeded!\n");
	}

	loop->gt_page_translation = g_hash_table_new(NULL, NULL);
	loop->gt_page_record_collection = g_hash_table_new_full(NULL,
	                                                        NULL,
	                                                        NULL,
	                                                        gt_destroy_page_record);
	loop->gt_ret_addr_mapping = g_hash_table_new_full(NULL,
	                                                  NULL,
	                                                  NULL,
	                                                  gt_restore_return_addr);

	vmi_pause_vm(loop->vmi);

	loop->os = vmi_get_ostype(loop->vmi);
	switch (loop->os) {
	case VMI_OS_LINUX:
		loop->os_functions = &os_functions_linux;
		break;
	case VMI_OS_WINDOWS:
		loop->os_functions = &os_functions_windows;
		break;
	default:
		fprintf(stderr, "unknown guest operating system\n");
		status = VMI_FAILURE;
		goto done;
	}

	loop->return_addr_width = vmi_get_address_width(loop->vmi);

	loop->xch = xc_interface_open(0, 0, 0);
	if (NULL == loop->xch) {
		fprintf(stderr, "failed to create xc interface\n");
                status = VMI_FAILURE;
		goto done;
	}

	rc = libxl_ctx_alloc(&loop->ctx, LIBXL_VERSION, 0, NULL);
	if (0 != rc) {
		fprintf(stderr, "failed to create libxl context\n");
                status = VMI_FAILURE;
		goto done;
	}

	loop->domid = ~0u;
	rc = libxl_name_to_domid(loop->ctx, guest_name, &loop->domid);
	if (0 != rc || ~0u == loop->domid) {
		fprintf(stderr, "failed to translate guest name to dom. ID\n");
                status = VMI_FAILURE;
		goto done;
	}

	loop->curr_mem_size = loop->init_mem_size = vmi_get_memsize(loop->vmi);
	if (0 == loop->init_mem_size) {
		fprintf(stderr, "failed to get guest memory size\n");
                status = VMI_FAILURE;
		goto done;
	}

	rc = xc_altp2m_set_domain_state(loop->xch, loop->domid, 1);
	if (rc < 0) {
		fprintf(stderr, "failed to enable altp2m on guest\n");
                status = VMI_FAILURE;
		goto done;
	}

	rc = xc_altp2m_create_view(loop->xch, loop->domid, 0, &loop->shadow_view);
	if (rc < 0) {
		fprintf(stderr, "failed to create view for shadow page\n");
                status = VMI_FAILURE;
		goto done;
	}

	vmi_resume_vm(loop->vmi);

done:
	if (VMI_SUCCESS != status) {
		gt_loop_free(loop);
		loop = NULL;
	}

	return loop;
}

/**
 * gt_loop_get_ostype:
 * @loop: a #GTLoop.
 *
 * Returns: the OS type of #GTLoop.
 **/
GTOSType
gt_loop_get_ostype(GTLoop *loop)
{
	switch (loop->os) {
	case VMI_OS_LINUX:
		return GT_OS_LINUX;
	case VMI_OS_WINDOWS:
		return GT_OS_WINDOWS;
	default:
		return GT_OS_UNKNOWN;
	}
}

/**
 * gt_loop_run:
 * @loop: a #GTLoop.
 *
 * Uses libvmi to complete the preparations necessary to trace a guest's system
 * calls. Runs @loop until gt_loop_quit() is called on @loop.
 */
void gt_loop_run(GTLoop *loop)
{
	int rc;

	vmi_pause_vm(loop->vmi);

	rc = xc_altp2m_switch_to_view(loop->xch, loop->domid, loop->shadow_view);
	if (rc < 0) {
		fprintf(stderr, "failed to enable shadow view\n");
		goto done;
	}

	if (!gt_set_up_generic_events(loop)) {
		goto done;
	}

	if (!gt_set_up_step_events(loop)) {
		goto done;
	}


	loop->return_addr = loop->os_functions->find_return_point_addr(loop);
	if (0 == loop->return_addr) {
		goto done;
	}

	loop->trampoline_addr = gt_find_trampoline_addr(loop);
	if (0 == loop->trampoline_addr) {
		fprintf(stderr, "could not find addr. of existing int 3 inst.\n");
		goto done;
	}

	vmi_resume_vm(loop->vmi);

	while(!gt_interrupted){
		status_t status = vmi_events_listen(loop->vmi, 500);
		if (status != VMI_SUCCESS) {
			fprintf(stderr, "error waiting for events\n");
			break;
		}
	}

done:

	return;
}

/**
 * gt_loop_quit:
 * @loop: a #GTLoop.
 *
 * Stops @loop from running. Any calls to gt_loop_run() for the loop will return.
 * This removes any modifications to the guest's memory and allows the guest
 * to run without instrumentation.
 */
void gt_loop_quit(GTLoop *loop)
{
	int status;

	vmi_pause_vm(loop->vmi);

	g_hash_table_remove_all(loop->gt_page_translation);
	g_hash_table_remove_all(loop->gt_ret_addr_mapping);
	g_hash_table_remove_all(loop->gt_page_record_collection);

	status = xc_altp2m_switch_to_view(loop->xch, loop->domid, 0);
	if (0 > status) {
		fprintf(stderr, "failed to reset EPT to point to default table\n");
	}

	vmi_resume_vm(loop->vmi);

	gt_interrupted = TRUE;
}

/**
 * gt_loop_free:
 * @loop: a #GTLoop.
 *
 * Free @loop and its associated memory. If the loop is currently running, then
 * gt_loop_quit() must first terminate the loop and remove the guest
 * instrumentation.
 */
void gt_loop_free(GTLoop *loop)
{
	if (NULL == loop) {
		goto done;
	}

	vmi_pause_vm(loop->vmi);

	g_hash_table_destroy(loop->gt_page_translation);
	g_hash_table_destroy(loop->gt_ret_addr_mapping);
	g_hash_table_destroy(loop->gt_page_record_collection);

	xc_altp2m_destroy_view(loop->xch, loop->domid, loop->shadow_view);
	xc_altp2m_set_domain_state(loop->xch, loop->domid, 0);
	/* TODO: find out why this isn't decreasing main memory on next run of guestrace */
	xc_domain_setmaxmem(loop->xch, loop->domid, loop->init_mem_size);

	libxl_ctx_free(loop->ctx);
	xc_interface_close(loop->xch);

	vmi_resume_vm(loop->vmi);

	vmi_destroy(loop->vmi);

	g_free(loop);

done:
	return;
}

/* Allocate a new page of memory in the guest's address space. */
static addr_t
gt_allocate_shadow_frame (GTLoop *loop)
{
	int rc;
	xen_pfn_t gfn = 0;
	uint64_t proposed_mem_size = loop->curr_mem_size + GT_PAGE_SIZE;

	rc = xc_domain_setmaxmem(loop->xch, loop->domid, proposed_mem_size);
	if (rc < 0) {
		fprintf(stderr,
		       "failed to increase memory size on guest to %lx\n",
		        proposed_mem_size);
		goto done;
	}

	loop->curr_mem_size = proposed_mem_size;

	rc = xc_domain_increase_reservation_exact(loop->xch, loop->domid,
	                                              1, 0, 0, &gfn);
	if (rc < 0) {
		fprintf(stderr, "failed to increase reservation on guest");
		goto done;
	}

	rc = xc_domain_populate_physmap_exact(loop->xch, loop->domid, 1, 0,
	                                          0, &gfn);
	if (rc < 0) {
		fprintf(stderr, "failed to populate GFN at 0x%lx\n", gfn);
		gfn = 0;
		goto done;
	}

done:
	return gfn;
}

/* Remove the breakpoint associated with paddr_record.  */
static status_t
gt_remove_breakpoint(gt_paddr_record *paddr_record) {
	uint8_t curr_inst;
	status_t status     = VMI_FAILURE;
	addr_t frame        = paddr_record->parent->frame;
	addr_t shadow_frame = paddr_record->parent->shadow_frame;
	addr_t offset       = paddr_record->offset;

	status = vmi_read_8_pa(paddr_record->parent->loop->vmi,
	                      (frame << GT_PAGE_OFFSET_BITS) + offset,
	                      &curr_inst);
	if (VMI_SUCCESS != status) {
		goto done;
	}

	status = vmi_write_8_pa(paddr_record->parent->loop->vmi,
	                       (shadow_frame << GT_PAGE_OFFSET_BITS) + offset,
	                       &curr_inst);

done:
	return status;
}

static void
gt_destroy_paddr_record (gpointer data) {
	gt_paddr_record *paddr_record = data;

	fprintf(stderr,
	       "destroying paddr record at shadow physical address %lx\n",
	       (paddr_record->parent->shadow_frame << GT_PAGE_OFFSET_BITS)
	      + paddr_record->offset);

	gt_remove_breakpoint(paddr_record);

	g_free(paddr_record);
}

/*
 * Ensure there exists a memory trap on the shadow page containing virtual
 * address va, and create a page record if it does not yet exist. Add a
 * physical-address record corresponding to va to the page record's collection
 * of children.
 */
static gt_paddr_record *
gt_setup_mem_trap (GTLoop *loop,
                   addr_t va,
                   GTSyscallFunc syscall_cb,
                   GTSysretFunc sysret_cb,
                   void *user_data)
{
	size_t ret;
	status_t status;
	gt_page_record  *page_record  = NULL;
	gt_paddr_record *paddr_record = NULL;

	addr_t pa = vmi_translate_kv2p(loop->vmi, va);
	if (0 == pa) {
		fprintf(stderr, "virtual addr. translation failed: %lx\n", va);
		goto done;
	}

	addr_t frame = pa >> GT_PAGE_OFFSET_BITS;
	addr_t shadow = (addr_t) g_hash_table_lookup(loop->gt_page_translation,
		                                     GSIZE_TO_POINTER(frame));
	addr_t shadow_offset = pa % GT_PAGE_SIZE;

	if (0 == shadow) {
		/* Record does not exist; allocate new page and create record. */
		shadow = gt_allocate_shadow_frame(loop);
		if (0 == shadow) {
			fprintf(stderr, "failed to allocate shadow page\n");
			goto done;
		}

		g_hash_table_insert(loop->gt_page_translation,
		                    GSIZE_TO_POINTER(frame),
		                    GSIZE_TO_POINTER(shadow));

		/* Activate in shadow view. */
		int xc_status = xc_altp2m_change_gfn(loop->xch,
		                                     loop->domid,
		                                     loop->shadow_view,
		                                     frame,
		                                     shadow);
		if (xc_status < 0) {
			fprintf(stderr, "failed to update shadow view\n");
			goto done;
		}
	}

	page_record = g_hash_table_lookup(loop->gt_page_record_collection,
	                                  GSIZE_TO_POINTER(shadow));
	if (NULL == page_record) {
		/* No record for this page yet; create one. */
		fprintf(stderr, "creating new page trap on 0x%lx -> 0x%lx\n",
		        shadow, frame);

		/* Copy page to shadow. */
		uint8_t buff[GT_PAGE_SIZE] = {0};
		ret = vmi_read_pa(loop->vmi,
		                  frame << GT_PAGE_OFFSET_BITS,
		                  buff,
		                  GT_PAGE_SIZE);
		if (GT_PAGE_SIZE != ret) {
			fprintf(stderr, "failed to read in syscall page\n");
			goto done;
		}

		ret = vmi_write_pa(loop->vmi,
		                   shadow << GT_PAGE_OFFSET_BITS,
		                   buff,
		                   GT_PAGE_SIZE);
		if (GT_PAGE_SIZE != ret) {
			fprintf(stderr, "failed to write to shadow page\n");
			goto done;
		}

		/* Initialize record of this page. */
		page_record              = g_new0(gt_page_record, 1);
		page_record->shadow_frame = shadow;
		page_record->frame       = frame;
		page_record->loop        = loop;
		page_record->children    = g_hash_table_new_full(NULL,
		                                       NULL,
		                                       NULL,
		                                       gt_destroy_paddr_record);

		g_hash_table_insert(loop->gt_page_record_collection,
		                    GSIZE_TO_POINTER(shadow),
		                    page_record);

		/* Establish callback on a R/W of this page. */
		vmi_set_mem_event(loop->vmi, frame, VMI_MEMACCESS_RW,
		                  loop->shadow_view);
	} else {
		/* We already have a page record for this page in collection. */
		paddr_record = g_hash_table_lookup(page_record->children,
		                                GSIZE_TO_POINTER(shadow_offset));
		if (NULL != paddr_record) {
			/* We have a paddr record too; done (no error). */
			goto done;
		}
	}

	/* Create physical-address record and add to page record. */
	paddr_record             = g_new0(gt_paddr_record, 1);
	paddr_record->offset     = shadow_offset;
	paddr_record->parent     = page_record;
	paddr_record->syscall_cb = syscall_cb;
	paddr_record->sysret_cb  = sysret_cb;
	paddr_record->data       = user_data;

	/* Write interrupt to our shadow page at the correct location. */
	status = vmi_write_8_pa(loop->vmi,
	                       (shadow << GT_PAGE_OFFSET_BITS) + shadow_offset,
	                       &GT_BREAKPOINT_INST);
	if (VMI_SUCCESS != status) {
		fprintf(stderr, "failed to write interrupt to shadow page\n");
		goto done;
	}

	g_hash_table_insert(page_record->children,
	                    GSIZE_TO_POINTER(shadow_offset),
	                    paddr_record);

done:
	/* TODO: Should undo loop (e.g., remove from hash tables) on error */
	return paddr_record;
}

/**
 * gt_loop_set_cb:
 * @loop: a #GTLoop.
 * @kernel_func: the name of a function in the traced kernel which implements
 * a system call.
 * @syscall_cb: a #GTSyscallFunc which will handle the named system call.
 * @sysret_cb: a #GTSysretFunc which will handle returns from the named
 * system call.
 * @user_data: optional data which the guestrace event loop will pass to each call of @syscall_cb
 *
 * Sets the callback functions associated with @kernel_func. Each time
 * processing a system call in the guest kernel calls @kernel_func,
 * The loop will invoke @syscall_cb with the parameters associated with the
 * call. When @kernel_func returns, the loop will invoke @sysret_cb.
 *
 * Returns: %TRUE on success, %FALSE on failure; an invalid @kernel_func
 * will cause the callback registration to fail.
 **/
gboolean gt_loop_set_cb(GTLoop *loop,
                        const char *kernel_func,
                        GTSyscallFunc syscall_cb,
                        GTSysretFunc sysret_cb,
                        void *user_data)
{
	gboolean fnval = FALSE;

	addr_t sysaddr;
	gt_paddr_record *syscall_trap;

	vmi_pause_vm(loop->vmi);

	sysaddr = vmi_translate_ksym2v(loop->vmi, kernel_func);
	if (0 == sysaddr) {
		goto done;
	}

	syscall_trap = gt_setup_mem_trap(loop, sysaddr, syscall_cb, sysret_cb, user_data);
	if (NULL == syscall_trap) {
		goto done;
	}

	fnval = TRUE;

done:
	vmi_resume_vm(loop->vmi);

	return fnval;
}

/**
 * gt_loop_set_cbs:
 * @loop: a #GTLoop.
 * @syscalls: an array of #GTSyscallCallback values, where each contains a
 * function name and corresponding #GTSyscallFunc and #GTSysretFunc.
 *
 * A convenience function which repeatedly invoke gt_loop_set_cb for each
 * callback defined in @syscalls. The @syscalls array must be terminated with
 * an #GTSyscallCallback with each field set to NULL.
 *
 * Returns: an integer which represents the number of callbacks
 * successfully set; an invalid function name in @syscalls will
 * cause the corresponding callback registration to fail.
 **/
int
gt_loop_set_cbs(GTLoop *loop, const GTSyscallCallback callbacks[])
{
	int count = 0;

	for (int i = 0; !gt_interrupted && callbacks[i].name; i++) {
		gboolean ok = gt_loop_set_cb(loop,
		                             callbacks[i].name,
		                             callbacks[i].syscall_cb,
		                             callbacks[i].sysret_cb,
		                             callbacks[i].user_data);
		if (ok) {
			count++;
		}
	}

	return count;
}

/*
 * Disassemble a page of memory beginning at <start> until
 * finding the correct mnemonic and op_str, returning the next address.
 * Note: op_str is optional.
 */
addr_t
_gt_find_addr_after_instruction (GTLoop *loop, addr_t start_v, char *mnemonic, char *ops)
{
	csh handle;
	cs_insn *inst;
	size_t count, offset = ~0;
	addr_t ret = 0;
	uint8_t code[GT_PAGE_SIZE];

	addr_t start_p = vmi_translate_kv2p(loop->vmi, start_v);
	if (0 == start_p) {
		fprintf(stderr, "failed to translate virtual start address to physical address\n");
		goto done;
	}

	/* Read kernel instructions into code. */
	status_t status = vmi_read_pa(loop->vmi, start_p, code, sizeof(code));
	if (VMI_FAILURE == status) {
		fprintf(stderr, "failed to read instructions from 0x%lx\n", start_p);
		goto done;
	}

	if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
		fprintf(stderr, "failed to open capstone\n");
		goto done;
	}

	/* Find CALL inst. and note address of inst. which follows. */
	count = cs_disasm(handle, code, sizeof(code), 0, 0, &inst);
	if (count > 0) {
		size_t i;
		for (i = 0; i < count; i++) {
			if (0 == strcmp(inst[i].mnemonic, mnemonic)
			 && (NULL == ops || 0 == strcmp(inst[i].op_str, ops))) {
				offset = inst[i + 1].address;
				break;
			}
		}
		cs_free(inst, count);
	} else {
		fprintf(stderr, "failed to disassemble system-call handler\n");
		goto done;
	}

	if (~0 == offset) {
		fprintf(stderr, "did not find call in system-call handler\n");
		goto done;
	}

	cs_close(&handle);

	ret = start_v + offset;

done:
	return ret;
}
