#include <Flash/Mpp/MPPTaskManager.h>
#include <Flash/Mpp/MinTSOScheduler.h>

namespace DB
{
constexpr UInt64 MAX_UINT64 = std::numeric_limits<UInt64>::max();
constexpr UInt64 OS_THREAD_SOFT_LIMIT = 100000;

MinTSOScheduler::MinTSOScheduler(UInt64 soft_limit, UInt64 hard_limit)
    : min_tso(MAX_UINT64)
    , thread_soft_limit(soft_limit)
    , thread_hard_limit(hard_limit)
    , used_threads(0)
    , log(&Poco::Logger::get("MinTSOScheduler"))
{
    auto cores = getNumberOfPhysicalCPUCores();
    active_set_soft_limit = (cores + 2) / 2; /// at least 1
    if (isDisabled())
    {
        LOG_FMT_INFO(log, "MinTSOScheduler is disabled!");
    }
    else if (thread_hard_limit <= thread_soft_limit || thread_hard_limit > OS_THREAD_SOFT_LIMIT) /// the general soft limit of OS threads is no more than 100000.
    {
        thread_hard_limit = 10000;
        thread_soft_limit = 5000;
        LOG_FMT_INFO(log, "hard limit {} should > soft limit {} and under maximum {}, so MinTSOScheduler set them as {}, {} by default, and active_set_soft_limit is {}.", hard_limit, soft_limit, OS_THREAD_SOFT_LIMIT, thread_hard_limit, thread_soft_limit, active_set_soft_limit);
    }
    else
    {
        LOG_FMT_INFO(log, "thread_hard_limit is {}, thread_soft_limit is {}, and active_set_soft_limit is {} in MinTSOScheduler.", thread_hard_limit, thread_soft_limit, active_set_soft_limit);
    }
}

bool MinTSOScheduler::tryToSchedule(const MPPTaskPtr & task, MPPTaskManager & task_manager)
{
    /// check whether this schedule is disabled or not
    if (isDisabled())
    {
        return true;
    }
    const auto & id = task->getId();
    auto query_task_set = task_manager.getQueryTaskSetWithoutLock(id.start_ts);
    if (nullptr == query_task_set || query_task_set->to_be_cancelled)
    {
        LOG_FMT_WARNING(log, "{} is scheduled with miss or cancellation.", id.toString());
        return true;
    }
    return scheduleImp(id.start_ts, query_task_set, task, false);
}

/// the cancelled query maybe hang, so trigger scheduling as needed.
void MinTSOScheduler::deleteCancelledQuery(const UInt64 tso, MPPTaskManager & task_manager)
{
    active_set.erase(tso);
    waiting_set.erase(tso);
    auto query_task_set = task_manager.getQueryTaskSetWithoutLock(tso);
    if (query_task_set) /// release all waiting tasks
    {
        while (!query_task_set->waiting_tasks.empty())
        {
            query_task_set->waiting_tasks.front()->scheduleThisTask();
            query_task_set->waiting_tasks.pop();
        }
    }

    /// NOTE: if the cancelled query hang, it will block the min_tso, possibly resulting in deadlock. so here force scheduling waiting tasks of the updated min_tso query.
    if (updateMinTSO(tso, true, "when cancelling it."))
    {
        scheduleWaitingQueries(task_manager);
    }
}

void MinTSOScheduler::deleteThenSchedule(const UInt64 tso, MPPTaskManager & task_manager)
{
    if (isDisabled())
    {
        return;
    }
    auto query_task_set = task_manager.getQueryTaskSetWithoutLock(tso);
    /// return back threads
    if (query_task_set)
    {
        used_threads -= query_task_set->used_threads;
        query_task_set->used_threads = 0;
        query_task_set->scheduled_task = 0;
    }
    LOG_FMT_INFO(log, "query {} (is min = {}) is deleted from active set {} left {} or waiting set {} left {}.", tso, tso == min_tso, active_set.find(tso) != active_set.end(), active_set.size(), waiting_set.find(tso) != waiting_set.end(), waiting_set.size());
    /// delete from working set and return threads for finished or cancelled queries
    active_set.erase(tso);
    waiting_set.erase(tso);
    updateMinTSO(tso, true, "as deleting it.");

    /// as deleted query release some threads, so some tasks would get scheduled.
    scheduleWaitingQueries(task_manager);
}

void MinTSOScheduler::scheduleWaitingQueries(MPPTaskManager & task_manager)
{
    /// schedule new tasks
    while (!waiting_set.empty())
    {
        auto current_query_id = *waiting_set.begin();
        auto query_task_set = task_manager.getQueryTaskSetWithoutLock(current_query_id);
        if (nullptr == query_task_set) /// silently solve this rare case
        {
            LOG_FMT_ERROR(log, "the waiting query {} is not in the task manager.", current_query_id);
            updateMinTSO(current_query_id, true, "as it is not in the task manager.");
            active_set.erase(current_query_id);
            waiting_set.erase(current_query_id);
            continue;
        }

        LOG_FMT_DEBUG(log, "query {} (is min = {}) with {} tasks is to be scheduled from waiting set (size = {}).", current_query_id, current_query_id == min_tso, query_task_set->waiting_tasks.size(), waiting_set.size());
        /// schedule tasks one by one
        while (!query_task_set->waiting_tasks.empty())
        {
            auto task = query_task_set->waiting_tasks.front();
            if (!scheduleImp(current_query_id, query_task_set, task, true))
                return;
            query_task_set->waiting_tasks.pop();
        }
        LOG_FMT_DEBUG(log, "query {} (is min = {}) is scheduled from waiting set (size = {}).", current_query_id, current_query_id == min_tso, waiting_set.size());
        waiting_set.erase(current_query_id); /// all waiting tasks of this query are fully active
    }
}

/// [directly schedule, from waiting set] * [is min_tso query, not] * [can schedule, can't] totally 8 cases.
bool MinTSOScheduler::scheduleImp(const UInt64 tso, const MPPQueryTaskSetPtr & query_task_set, const MPPTaskPtr & task, const bool isWaiting)
{
    auto needed_threads = task->getNeededThreads();
    auto check_for_new_min_tso = tso <= min_tso && used_threads + needed_threads <= thread_hard_limit;
    auto check_for_not_min_tso = (active_set.size() < active_set_soft_limit || tso <= *active_set.rbegin()) && (used_threads + needed_threads <= thread_soft_limit);
    if (check_for_new_min_tso || check_for_not_min_tso)
    {
        updateMinTSO(tso, false, isWaiting ? "from the waiting set" : "when directly schedule it");
        active_set.insert(tso);
        ++query_task_set->scheduled_task;
        query_task_set->used_threads += needed_threads;
        used_threads += needed_threads;
        if (isWaiting)
            task->scheduleThisTask();
        LOG_FMT_INFO(log, "{} is scheduled (active set size = {}) due to available threads {}, after applied for {} threads, used {} of the thread {} limit {}.", task->getId().toString(), active_set.size(), isWaiting ? " from the waiting set" : " directly", needed_threads, used_threads, min_tso == tso ? "hard" : "soft", min_tso == tso ? thread_hard_limit : thread_soft_limit);
        return true;
    }
    else
    {
        if (tso <= min_tso) /// the min_tso query should fully run, otherwise throw errors here.
        {
            auto msg = fmt::format("threads are unavailable for the query {} ({} min_tso {}) {}, need {}, but used {} of the thread hard limit {}, {} active and {} waiting queries.", tso, tso == min_tso ? "is" : "is newer than", min_tso, isWaiting ? "from the waiting set" : "when directly schedule it", needed_threads, used_threads, thread_hard_limit, active_set.size(), waiting_set.size());
            LOG_FMT_ERROR(log, "{}", msg);
            throw Exception(msg);
        }
        if (!isWaiting)
        {
            waiting_set.insert(tso);
            query_task_set->waiting_tasks.push(task);
        }
        LOG_FMT_INFO(log, "threads are unavailable for the query {} or active set is full (size =  {}), need {}, but used {} of the thread soft limit {},{} waiting set size = {}", tso, active_set.size(), needed_threads, used_threads, thread_soft_limit, isWaiting ? "" : " put into", waiting_set.size());
        return false;
    }
}

/// if return true, then need to schedule the waiting tasks of the min_tso.
bool MinTSOScheduler::updateMinTSO(const UInt64 tso, const bool retired, const String msg)
{
    auto old_min_tso = min_tso;
    bool force_scheduling = false;
    if (retired)
    {
        if (tso == min_tso) /// elect a new min_tso from all queries.
        {
            min_tso = active_set.empty() ? MAX_UINT64 : *active_set.begin();
            min_tso = waiting_set.empty() ? min_tso : std::min(*waiting_set.begin(), min_tso);
            force_scheduling = waiting_set.find(min_tso) != waiting_set.end(); /// if this min_tso has waiting tasks, these tasks should force being scheduled.
        }
    }
    else
    {
        min_tso = std::min(tso, min_tso);
    }
    if (min_tso != old_min_tso) /// if min_tso == MAX_UINT64 and the query tso is not to be cancelled, the used_threads, active_set.size() and waiting_set.size() must be 0.
        LOG_FMT_INFO(log, "min_tso query is updated from {} to {} {}, used threads = {}, {} active and {} waiting queries.", old_min_tso, min_tso, msg, used_threads, active_set.size(), waiting_set.size());
    return force_scheduling;
}
} // namespace DB