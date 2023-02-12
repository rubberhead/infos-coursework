/*
 * The Multi-Queue Priority-Value Task Scheduler
 * 
 * B171926
*/

#include <infos/kernel/sched.h>
#include <infos/kernel/sched-entity.h>
#include <infos/kernel/thread.h>
#include <infos/kernel/log.h>
#include <infos/util/list.h>
#include <infos/util/lock.h>

#define TIME_QUANTUM SchedulingEntity::EntityRuntime(5000000); // 5ms

/* Initial priority values for each priority level */
#define REALTIME_PRIO_BASE_VAL     63; 
#define INTERACTIVE_PRIO_BASE_VAL 127; 
#define NORMAL_PRIO_BASE_VAL      191; 
#define DAEMON_PRIO_BASE_VAL      255; 

/* Priority value increment deltas for each priority level */
#define REALTIME_PRIO_INCR_DELTA    8; 
#define INTERACTIVE_PRIO_INCR_DELTA 4;
#define NORMAL_PRIO_INCR_DELTA      2; 
#define DAEMON_PRIO_INCR_DELTA      1;

using namespace infos::kernel; 
using namespace infos::util; 

struct RunqueueEntry 
{
public: 
    SchedulingEntity* entity; 
    uint8_t priority_value; 

    RunqueueEntry() = default; // [FIXME] Should be `delete` but `list.h` complains otherwise, not sure what's up
    explicit RunqueueEntry(SchedulingEntity* entity) 
    {
        this->entity = entity; 
        switch (entity->priority()) {
            case SchedulingEntityPriority::REALTIME:
                this->priority_value = REALTIME_PRIO_BASE_VAL;
                this->_incr_delta    = REALTIME_PRIO_INCR_DELTA;  
                break;
            case SchedulingEntityPriority::INTERACTIVE: 
                this->priority_value = INTERACTIVE_PRIO_BASE_VAL;
                this->_incr_delta    = INTERACTIVE_PRIO_INCR_DELTA;  
                break; 
            case SchedulingEntityPriority::NORMAL: 
                this->priority_value = NORMAL_PRIO_BASE_VAL; 
                this->_incr_delta    = NORMAL_PRIO_INCR_DELTA; 
                break; 
            case SchedulingEntityPriority::DAEMON:
                this->priority_value = DAEMON_PRIO_BASE_VAL; 
                this->_incr_delta    = DAEMON_PRIO_INCR_DELTA; 
                break; 
        }
    }

    /**
     * @brief Bounded-decrement priority value of this scheduling entity.
     * 
     * @return true if `priority_value` already at 0
     * @return false if otherwise
     */
    bool decrement() 
    {
        if (this->priority_value == 0) return false;
        this->priority_value -= 1; 
        return true; 
    }

    /**
     * @brief Bounded-increment priority value of this scheduling entity.
     * 
     * @return true if `priority_value` incremented to `__UINT8_MAX__` == 255
     * @return false if otherwise
     */
    bool increment() 
    {
        if (__UINT8_MAX__ - this->priority_value < this->_incr_delta) {
            this->priority_value = __UINT8_MAX__; 
            return false; 
        }
        this->priority_value += this->_incr_delta; 
        return true; 
    }

    RunqueueEntry& operator=(const RunqueueEntry& rhs) = default; // Copy unless equal ptr
    // RunqueueEntry& operator=(RunqueueEntry&& rhs) = default;      // Move assignment unless 

    inline bool operator==(const RunqueueEntry& rhs) 
    {
        return (this->entity == rhs.entity && this->priority_value == rhs.priority_value); 
    }

    inline bool operator!=(const RunqueueEntry& rhs)
    {
        return !(*this == rhs); 
    }
    

private: 
    uint8_t _incr_delta; // [FIXME] to const... somehow
}; 
typedef List<RunqueueEntry> RunQueue; 

/**
 * @brief The Multi-Queue Priority-Value Scheduler.
 * 
 * This scheduler emulates the behavior of Multilevel Feedback Queue algorithm, 
 * but preserves each task's priority level and utilizes a more fine-grained control 
 * over each task's priority value -- a variable dependent on (1) its priority level, 
 * (2) its wait time, and (3) the amount of time it expires its time quantum. 
 */
class MultiQueuePriorityValueScheduler : public SchedulingAlgorithm
{
public:
    /**
     * @brief Returns the friendly name of the algorithm for debugging 
     * and selection purposes. 
     * 
     * @return const char* name of the algorithm. 
     */
    const char* name() const override { return "mqpv"; }

    /**
     * @brief Called during scheduler initialization.
     */
    void init()
    {
        sched_log.messagef(
            LogLevel::DEBUG, 
            "[%s] Initialized scheduling algorithm MQPV", 
            name()
        );
    }

    /**
     * @brief Called when a scheduling entity becomes eligible for running.
     * 
     * @param entity 
     */
    void add_to_runqueue(SchedulingEntity& entity) override 
    {
        RunqueueEntry entry = RunqueueEntry(&entity); 

        UniqueIRQLock lock = UniqueIRQLock(); 
        size_t idx = entity.priority(); 
        runqueues[idx].push(entry); 
        lock.~UniqueIRQLock(); 
    }

    /**
     * @brief Called when a scheduling entity is no longer eligible for running.
     * 
     * @param entity 
     */
    void remove_from_runqueue(SchedulingEntity& entity) override
    {
        UniqueIRQLock lock = UniqueIRQLock(); 
        size_t idx = entity.priority(); 

        // [TODO] Use map to store entries -- or not? 
        // You could expect each entity removed to be at front of queue.
        const RunqueueEntry* corresponding_entry = NULL; 
        for (const RunqueueEntry entry : runqueues[idx]) {
            if (entry.entity == &entity) {
                corresponding_entry = &entry; 
                break; 
            }
        }
        assert(corresponding_entry != NULL); 
        runqueues[idx].remove(*corresponding_entry); 

        lock.~UniqueIRQLock(); 
    }

    /**
     * Called every time a scheduling event occurs, to cause the next eligible entity
     * to be chosen.  The next eligible entity might actually be the same entity, if
     * e.g. its timeslice has not expired.
     * 
     * @return SchedulingEntity* 
     */
    SchedulingEntity *pick_next_entity() override 
    {
        RunqueueEntry* firsts[4]; 

        // Iterate over each rq
        for (size_t i = 0; i < 4; i++) {
            RunQueue& rq = runqueues[i]; 
            if (rq.count() == 0) continue; 

            // Select first
            auto top_entry = rq.pop(); 
            if (top_entry.entity != last_sched_entity_ptr) { // new entry
                firsts[i] = &top_entry;  
                rq.enqueue(top_entry); // Append at front
                continue; 
            } else { // selected again
                top_entry.increment();      // Update priority value
                rq.append(top_entry);       // Append at back (since RR), proceed
            }

            // Select next (or sole entry)
            top_entry = rq.pop(); // [FIXME] Use move operator instead of copy
            firsts[i] = &top_entry; 
            rq.enqueue(top_entry); 
        }
        
        // Iterate over firsts, select min
        auto scheduled_entry_ptr = firsts[0]; 
        for (size_t i = 1; i < 4; i++) {
            if (firsts[i] == NULL) continue; 
            if (firsts[i]->priority_value < scheduled_entry_ptr->priority_value) {
                scheduled_entry_ptr = firsts[i]; 
            }
        }

        // Decrement all non-selected tasks
        for (auto entry_ptr : firsts) {
            if (entry_ptr == scheduled_entry_ptr) continue; 
            entry_ptr->decrement(); 
        }

        // unwrap
        auto entity = (scheduled_entry_ptr == NULL) ? NULL : scheduled_entry_ptr->entity;
        last_sched_entity_ptr = entity; 
        return entity; 
    }

private:
    RunQueue runqueues[4]; // Idx 0 -- 3 represent 4 lvls of priority
    const SchedulingEntity* last_sched_entity_ptr; // [UNSAFE] Will dangle! Never dereference. 
}; 

RegisterScheduler(MultiQueuePriorityValueScheduler); 