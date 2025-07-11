/****************************************************************************
 *
 * Copyright 2024 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * arch/arm/src/armv7-a/arm_cpuhotplug.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <stdint.h>
#include <assert.h>
#include <debug.h>

#include <tinyara/arch.h>
#include <tinyara/sched.h>
#include <tinyara/sched_note.h>

#include "up_internal.h"
#include "cp15_cacheops.h"
#include "gic.h"
#include "sched/sched.h"
#include "barriers.h"
#include "smp.h"
#include "arm.h"

#ifdef CONFIG_CPU_HOTPLUG
/* cpu hotplug flag for each core */
static volatile cpu_state_t g_cpuhp_flag[CONFIG_SMP_NCPUS];

void up_set_cpu_state(uint32_t CoreID, cpu_state_t NewStatus)
{
	g_cpuhp_flag[CoreID] = NewStatus;
	ARM_DSB();
}

cpu_state_t up_get_cpu_state(uint32_t CoreID)
{
	return g_cpuhp_flag[CoreID];
}

/****************************************************************************
 * Name: arm_hotplug_handler
 *
 * Description:
 *   This is the handler for SGI4.  It performs the following operations:
 *
 *   1. It saves the current task state at the head of the current assigned
 *      task list.
 *   2. CPU goes to low power state (ie. CPU Shutdown)
 *   3. CPU returns from low power, restoring the state of the new task at the
 *      head of the ready to run list. (ie. We assume the SGI to be triggered
 *      by the primary core)
 *
 * Input Parameters:
 *   Standard interrupt handling
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/
int arm_hotplug_handler(int irq, void *context, void *arg)
{
	/* Remark: Currently this handler only handles case which after secondary core
	           shutdown. we will expect primary core to shutdown as well (ie. Not handling tasks anymore)
			   It is because current kernel is not aware of secondary core has shutdown 
			   (ie. TizenRT Kernel logic is not ready to handle such case yet)
			   Thus, we will be facing a deadlock when cpuA fire SGI2 to cpuB (ie. pausing CPU)
	*/

	/* Set secondary core to hotplug state */
	int cpu = this_cpu();
	up_set_cpu_state(cpu, CPU_HOTPLUG);
	/* Save the tcb state */
	struct tcb_s *rtcb = this_task();
	arm_savestate(rtcb->xcp.regs);
	rtcb->task_state = TSTATE_TASK_ASSIGNED;
	CURRENT_REGS = NULL;

	/* MINOR: PSCI is a general interface for controlling power of cortex-A cores 
	   It is one of the feature in arm-trusted-firmware (ie. Trustzone-A)
	   Currently, the definitions are provided by chip specific arch layer
	   If the cpu off operation failed, we should face a problem during wakeup
	   booting the secondary core, thus we can ignore the return value here first
	*/
	/* Shut down the secondary core */
   	(void)psci_cpu_off(); 
	return OK;
}

/****************************************************************************
 * Name: up_cpu_hotplug
 *
 * Description:
 *   Send signal for target CPU to enter hotplug mode.
 *
 *   This function is called after up_cpu_pause in order to ensure
 *   the target CPU enter hotplug when executing idle thread
 *
 * Input Parameters:
 *   cpu - The index of the CPU being hotplug.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called from within a critical section; up_cpu_pause() must have
 *   previously been called within the same critical section. This
 *   function should only be called by primary core which conducts PM logic.
 *
 ****************************************************************************/
void up_cpu_hotplug(int cpu)
{
	DEBUGASSERT(cpu >= 0 && cpu < CONFIG_SMP_NCPUS && cpu != this_cpu());

	/* Ensure that hotplug target CPU's systick PPI stops firing to prevent deadlock condition */
	up_systimer_pause(cpu);

	/* Fire SGI for cpu to enter hotplug */
	arm_cpu_sgi(GIC_IRQ_SGI4, (1 << cpu));
}

#endif /* CONFIG_CPU_HOTPLUG */
