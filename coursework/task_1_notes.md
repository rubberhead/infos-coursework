# Problem w/ `MQ-PRIO`
- *Starvation*: low priority jobs may only run after the OS finished scheduling all high priority jobs. For a runtime environment with neverending stream of jobs of different priority levels, this could result in low priority jobs never getting the chance to run. 
- *Deadlock*: if a low priority job holds a lock that a high priority job needs, the high priority job is always scheduled to run despite being unable to compute, while the low priority job never gets to run because high prioirty job exists.

# Solution
Both problems stem from the rigid framework under which `MQ-PRIO` operates -- wherein high priority jobs MUST preceed low priority jobs with little room for alteration except via OS or user intervention through other components. The `MLFQ` algorithm alleviates this issue somewhat with the downside of making alterations to each job's priority level on-the-fly -- which could pose problems for OS telemetry. 

We propose an alternative to both algorithms: the `Multiple-Queue Priority-Level` algorithm for OS scheduling. We encapsulate each job with a semaphore that indicates its priority level at the time of this scheduling operation. At each scheduling call, one job from each priority queue is obtained and the task with the smallest semaphore value is selected for scheduling. At the same time the rest of the unselected tasks shall have their semaphore level decremented. 

If the scheduled job eventually ends up unfinished, its semaphore level is incremented according to its priority level -- the higher the priority level of the job is, the more its semaphore level is incremented (as high-priority level tasks are expected to run for shorter). 

Optionally the kernel tuner can opt to use different time quantums for each semaphore level threshold as opposed to priority level -- this (should) results in higher CPU utilization at the cost of response time, as tasks that run longer are now guaranteed to run for longer at the cost of number of tasks run at a given time slice. 