/*
 * The Priority Task Scheduler
 * SKELETON IMPLEMENTATION TO BE FILLED IN FOR TASK 1
 * ==================================================================
 * Fin.
 * 
 * B171926
 */

#include <infos/kernel/sched.h>
#include <infos/kernel/thread.h>
#include <infos/kernel/log.h>
#include <infos/util/list.h>
#include <infos/util/lock.h>

#include <infos/kernel/sched-entity.h>
#define TIME_QUANTUM SchedulingEntity::EntityRuntime(5000000); // 5ms

using namespace infos::kernel;
using namespace infos::util;

typedef List<SchedulingEntity *> RunQueue; 

/**
 * A Multiple Queue priority scheduling algorithm
 */
class MultipleQueuePriorityScheduler : public SchedulingAlgorithm
{
public:
    /**
     * Returns the friendly name of the algorithm, for debugging and selection purposes.
     */
    const char* name() const override { return "mq"; }

    /**
     * Called during scheduler initialisation.
     */
    void init()
    {
        sched_log.messagef(
            LogLevel::DEBUG, 
            "[%s] Initialized scheduling algorithm with time quantum %d ns", 
            name(), 
            time_quantum
        );
    }

    /**
     * Called when a scheduling entity becomes eligible for running.
     * @param entity
     */
    void add_to_runqueue(SchedulingEntity& entity) override
    {
        UniqueIRQLock lock = UniqueIRQLock(); 
        switch (entity.priority()) {
            case SchedulingEntityPriority::REALTIME: 
                runqueues[0].push(&entity); 
                break;
            case SchedulingEntityPriority::INTERACTIVE: 
                runqueues[1].push(&entity); 
                break;
            case SchedulingEntityPriority::NORMAL: 
                runqueues[2].push(&entity); 
                break;
            case SchedulingEntityPriority::DAEMON: 
                runqueues[3].push(&entity); 
                break; 
        }
        lock.~UniqueIRQLock(); 
    }

    /**
     * Called when a scheduling entity is no longer eligible for running.
     * @param entity
     */
    void remove_from_runqueue(SchedulingEntity& entity) override
    {
        UniqueIRQLock lock = UniqueIRQLock(); 
        switch (entity.priority()) {
            case SchedulingEntityPriority::REALTIME: 
                runqueues[0].remove(&entity); 
                break; 
            case SchedulingEntityPriority::INTERACTIVE:
                runqueues[1].remove(&entity); 
                break; 
            case SchedulingEntityPriority::NORMAL: 
                runqueues[2].remove(&entity);
                break; 
            case SchedulingEntityPriority::DAEMON: 
                runqueues[3].remove(&entity); 
                break; 
        }
        lock.~UniqueIRQLock(); 
    }

    /**
     * Called every time a scheduling event occurs, to cause the next eligible entity
     * to be chosen.  The next eligible entity might actually be the same entity, if
     * e.g. its timeslice has not expired.
     */
    SchedulingEntity *pick_next_entity() override
    {
        for (RunQueue& rq : runqueues) { // From highest to lowest priority
            if (rq.count() == 0) continue; 

            // Select first
            auto top_entity = rq.first(); 
            if (rq.count() == 1) return top_entity; 
            if (top_entity == last_entity_ptr && top_entity->cpu_runtime() < last_entity_runtime_limit) {
                // Ran for less than `time_quantum`
                return top_entity; 
            } else {
                rq.append(rq.pop()); // append at back, proceed
            }

            // Select next
            top_entity = rq.first(); 
            last_entity_ptr = top_entity; 
            last_entity_runtime_limit = top_entity->cpu_runtime() + time_quantum;
            return top_entity; 
        }

        return NULL; 
    }

private: 
    SchedulingEntity::EntityRuntime time_quantum = TIME_QUANTUM; 
    RunQueue runqueues[4]; // Idx 0 -- 3 represent 4 lvls of priority
    SchedulingEntity* last_entity_ptr = nullptr; 
    SchedulingEntity::EntityRuntime last_entity_runtime_limit; 
};

/* --- DO NOT CHANGE ANYTHING BELOW THIS LINE --- */

RegisterScheduler(MultipleQueuePriorityScheduler);
