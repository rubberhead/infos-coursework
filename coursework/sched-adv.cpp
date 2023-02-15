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

/* Initial priority values for each priority level */
#define REALTIME_PRIO_BASE_VAL     63; 
#define INTERACTIVE_PRIO_BASE_VAL 127; 
#define NORMAL_PRIO_BASE_VAL      191; 
#define DAEMON_PRIO_BASE_VAL      255; 

/* Priority value increment/decrement deltas for each priority level */
#define REALTIME_PRIO_DELTA    8; 
#define INTERACTIVE_PRIO_DELTA 4;
#define NORMAL_PRIO_DELTA      2; 
#define DAEMON_PRIO_DELTA      1;

using namespace infos::kernel; 
using namespace infos::util; 

struct RunqueueEntry 
{
public: 
    SchedulingEntity* entity; 
    uint8_t priority_value; 

    /* [DONTUSE] Should be `delete` but `list.h` complains otherwise. */
    RunqueueEntry() = default; 

    /*
     * [DONTUSE] Should be `delete` but `list.h` complains otherwise. 
     * This (imo) is due to `list.h` using copy constructors for its element generic type. 
    */
    RunqueueEntry(const RunqueueEntry&) = default; 
    RunqueueEntry(RunqueueEntry&&) = default;
    RunqueueEntry(SchedulingEntity* entity) 
    {
        this->entity = entity; 
        switch (entity->priority()) {
            case SchedulingEntityPriority::REALTIME:
                this->priority_value  = REALTIME_PRIO_BASE_VAL;
                this->_priority_delta = REALTIME_PRIO_DELTA;  
                break;
            case SchedulingEntityPriority::INTERACTIVE: 
                this->priority_value  = INTERACTIVE_PRIO_BASE_VAL;
                this->_priority_delta = INTERACTIVE_PRIO_DELTA;  
                break; 
            case SchedulingEntityPriority::NORMAL: 
                this->priority_value  = NORMAL_PRIO_BASE_VAL; 
                this->_priority_delta = NORMAL_PRIO_DELTA; 
                break; 
            case SchedulingEntityPriority::DAEMON:
                this->priority_value  = DAEMON_PRIO_BASE_VAL; 
                this->_priority_delta = DAEMON_PRIO_DELTA; 
                break; 
        }
    }

    /**
     * @brief Bounded-decrement priority value of this scheduling entity.
     * 
     * @return Resultant `priority_value`
     */
    uint8_t decrement() 
    {
        if (this->_priority_delta >= this->priority_value) {
            this->priority_value = 0; 
        } else {
            this->priority_value -= this->_priority_delta; 
        }
        return this->priority_value; 
    }

    /**
     * @brief Bounded-increment priority value of this scheduling entity.
     * 
     * @return Resultant `priority_value`
     */
    uint8_t increment() 
    {
        if (__UINT8_MAX__ - this->priority_value <= this->_priority_delta) {
            this->priority_value = __UINT8_MAX__; 
        } else {
            this->priority_value += this->_priority_delta; 
        }
        return this->priority_value; 
    }

    RunqueueEntry& operator=(const RunqueueEntry& rhs) = default; // Copy unless equal ptr
    RunqueueEntry& operator=(RunqueueEntry&& rhs) = default;      // Move unless equal ptr

    inline bool operator==(const RunqueueEntry& rhs) 
    {
        return (this->entity == rhs.entity); 
    }

    inline bool operator!=(const RunqueueEntry& rhs)
    {
        return !(*this == rhs); 
    }
    

private: 
    uint8_t _priority_delta; // [FIXME] to const... somehow
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
    const char* name() const override { return "adv"; }

    /**
     * @brief Called during scheduler initialization.
     */
    void init()
    {
        // Nothing...
    }

    /**
     * @brief Called when a scheduling entity becomes eligible for running.
     * 
     * @param entity 
     */
    void add_to_runqueue(SchedulingEntity& entity) override 
    {
        UniqueIRQLock(); 

        RunqueueEntry entry (&entity); 
        size_t idx = entity.priority(); 
        runqueues[idx].push(entry); 

        sched_log.messagef(
            LogLevel::IMPORTANT, 
            "[%s] Added entry @ 0x%x for {@ 0x%x | P-lvl: %d}", 
            name(), 
            &runqueues[idx].last(), 
            entry.entity, 
            entity.priority()
        );
    }

    /**
     * @brief Called when a scheduling entity is no longer eligible for running.
     * 
     * @param entity 
     */
    void remove_from_runqueue(SchedulingEntity& entity) override
    {
        UniqueIRQLock(); 
        size_t idx = entity.priority(); 

        // [TODO] Use map to store entries -- or not? 
        // You could expect each entity removed to be at front of queue.
        const RunqueueEntry* corresponding_entry = NULL; 
        for (const RunqueueEntry& entry : runqueues[idx]) {
            if (entry.entity == &entity) {
                corresponding_entry = &entry; 
                break; 
            }
        }
        assert(corresponding_entry != NULL); 
        runqueues[idx].remove(*corresponding_entry); 
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
        // Move semantics DOES NOT IMPLY that the memory addresses of l-rvalue would be the same!!!!!!!!!
        // This is (I guess?) due to VA-PA abstraction, which does that job for you (diff VA -> same PA, first now has no ownership). 
        // Cost differential from copy semantics comes from diff VA -> diff PA in comparison. 
        // (I guess this course really is a little useful for you? Yes to internal knowledge, no to premature optimization)
        RunqueueEntry firsts[4] = {NULL, NULL, NULL, NULL}; 

        // Iterate over each rq to fill `firsts`
        for (size_t i = 0; i < 4; i++) {
            RunQueue& rq = runqueues[i]; 
            if (rq.count() == 0) continue; 

            // Select first
            RunqueueEntry top_entry = rq.pop(); 
            if (top_entry.entity != _last_entity_ptr) { // new entry
                firsts[i] = top_entry; 
                rq.enqueue(top_entry); // Copy-enqueue
            } else {                                    // selected again (across all rqs)
                uint8_t prev = top_entry.priority_value; 
                uint8_t curr = top_entry.increment();            // Update priority value
                assert(prev == 255 || prev != curr); 
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] Entry for @ 0x%x makes P-val change %d -> %d", 
                    name(), 
                    top_entry.entity, 
                    prev, 
                    curr
                ); 

                rq.append(top_entry);                           // Append at back (since RR)
                firsts[i] = rq.pop(); 
                rq.enqueue(top_entry);
            }
        }

        // Logging purposes =======================================================================
        sched_log.messagef(
            LogLevel::IMPORTANT, 
            "[%s] BEFORE ITERATION", 
            name()
        ); 
        for (size_t i = 0; i < 4; i++) {
            auto entry_ptr = firsts[i]; 
            if (entry_ptr == NULL) {
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] First at P-lvl %d is NULL. Total rq size = %d", 
                    name(), 
                    i, 
                    runqueues[i].count()
                ); 
            } else {
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] First at P-lvl %d has P-val %d and point to entity @ 0x%x. Total rq size = %d", 
                    name(), 
                    i, 
                    entry_ptr->priority_value, // DOES NOT CHANGE!
                    entry_ptr->entity, 
                    runqueues[i].count()
                ); 
            }
        }
        // ==========================================================================================
        
        RunqueueEntry* scheduled_entry_ptr = NULL; 
        // Iterate over firsts, select min or NULL (if all NULL)
        for (const auto entry_ptr : firsts) {
            if (entry_ptr == NULL) {
                continue;
            }
            if (scheduled_entry_ptr == NULL ||
                entry_ptr->priority_value < scheduled_entry_ptr->priority_value
            ) {
                scheduled_entry_ptr = entry_ptr; 
            }
        }
        if (scheduled_entry_ptr == NULL) return NULL; 

        // Decrement all non-selected tasks if non-NULL
        for (auto entry_ptr : firsts) {
            if (entry_ptr != NULL && entry_ptr != scheduled_entry_ptr) {
                uint8_t prev = entry_ptr->priority_value; 
                uint8_t curr = entry_ptr->decrement(); 
                assert(prev == 0 || prev != curr); 
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] Entry for @ 0x%x makes P-val change %d -> %d", 
                    name(), 
                    entry_ptr->entity, 
                    prev, 
                    curr
                ); 
            }
        }

        // Logging purposes ========================================================================
        sched_log.messagef(
            LogLevel::IMPORTANT, 
            "[%s] AFTER ITERATION", 
            name()
        ); 
        for (size_t i = 0; i < 4; i++) {
            auto entry_ptr = firsts[i]; 
            if (entry_ptr == NULL) {
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] First at P-lvl %d is NULL. Total rq size = %d", 
                    name(), 
                    i, 
                    runqueues[i].count()
                ); 
            } else {
                sched_log.messagef(
                    LogLevel::DEBUG, 
                    "[%s] First at P-lvl %d has P-val %d and point to entity @ 0x%x. Total rq size = %d", 
                    name(), 
                    i, 
                    entry_ptr->priority_value, // DOES NOT CHANGE!
                    entry_ptr->entity, 
                    runqueues[i].count()
                ); 
            }
        }
        // =============================================================================================

        // unwrap
        assert(scheduled_entry_ptr != NULL);
        assert(scheduled_entry_ptr->entity != NULL); 
        sched_log.messagef(
            LogLevel::IMPORTANT, 
            "[%s] Selected entity {@ 0x%x | P-lvl: %d, P-val: %d}", 
            name(), 
            scheduled_entry_ptr->entity, 
            scheduled_entry_ptr->entity->priority(), 
            scheduled_entry_ptr->priority_value
        ); 
        _last_entity_ptr = scheduled_entry_ptr->entity; // Update last selection
        return scheduled_entry_ptr->entity; 
    }

private:
    RunQueue runqueues[4]; // Idx 0 -- 3 represent 4 lvls of priority
    const SchedulingEntity* _last_entity_ptr; // [UNSAFE] Will dangle! Never dereference. 
}; 

RegisterScheduler(MultiQueuePriorityValueScheduler); 