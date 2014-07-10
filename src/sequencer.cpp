/**
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <thread>
#include <boost/heap/skew_heap.hpp>
#include "akumuli_def.h"
#include "sequencer.h"
#include "util.h"

#define PARAM_ID 1
#define TIMESTMP 0

using namespace std;

namespace Akumuli {

template<typename RunType>
bool top_element_less(const RunType& x, const RunType& y)
{
    return x.back() < y.back();
}

template<typename RunType>
bool top_element_more(const RunType& x, const RunType& y)
{
    return top_element_less(y, x);
}

TimeSeriesValue::TimeSeriesValue() {}

TimeSeriesValue::TimeSeriesValue(TimeStamp ts, ParamId id, EntryOffset offset)
    : key_(ts, id)
    , value(offset)
{
}

bool operator < (TimeSeriesValue const& lhs, TimeSeriesValue const& rhs) {
    return lhs.key_ < rhs.key_;
}

// Sequencer

Sequencer::Sequencer(PageHeader const* page, TimeDuration window_size)
    : window_size_(window_size)
    , page_(page)
    , top_timestamp_()
    , checkpoint_(0u)
    , run_lock_flags_(RUN_LOCK_FLAGS_SIZE)
{
    if (window_size.value <= 0) {
        throw runtime_error("window size must greather than zero");
    }

    key_.push_back(TimeSeriesValue());

    for(auto& flag: run_lock_flags_) {
        flag.clear();
    }
}

//! Checkpoint id = ⌊timestamp/window_size⌋
uint32_t Sequencer::get_checkpoint_(TimeStamp ts) const {
    // TODO: use fast integer division (libdivision or else)
    return ts.value / window_size_.value;
}

//! Convert checkpoint id to timestamp
TimeStamp Sequencer::get_timestamp_(uint32_t cp) const {
    return TimeStamp::make(cp*window_size_.value);
}

// move sorted runs to ready_ collection
void Sequencer::make_checkpoint_(uint32_t new_checkpoint, Lock& lock) {
    if(!lock.try_lock()) {
        return;
    }
    lock_all_runs();  // stop all consurrent searches
    auto old_top = get_timestamp_(checkpoint_);
    checkpoint_ = new_checkpoint;
    if (!ready_.empty()) {
        throw runtime_error("sequencer invariant is broken");
    }
    vector<SortedRun> new_runs;
    for (auto& sorted_run: runs_) {
        auto it = lower_bound(sorted_run.begin(), sorted_run.end(), TimeSeriesValue(old_top, AKU_LIMITS_MAX_ID, 0));
        if (it == sorted_run.begin()) {
            // all timestamps are newer than old_top, do nothing
            new_runs.push_back(move(sorted_run));
            continue;
        } else if (it == sorted_run.end()) {
            // all timestamps are older than old_top, move them
            ready_.push_back(move(sorted_run));
        } else {
            // it is in between of the sorted run - split
            SortedRun run;
            copy(sorted_run.begin(), it, back_inserter(run));  // copy old
            ready_.push_back(move(run));
            run.clear();
            copy(it, sorted_run.end(), back_inserter(run));  // copy new
            new_runs.push_back(move(run));
        }
    }
    swap(runs_, new_runs);
    unlock_all_runs();
    atomic_thread_fence(memory_order_acq_rel);
    return;
}

/** Check timestamp and make checkpoint if timestamp is large enough.
  * @returns error code and flag that indicates whether or not new checkpoint is created
  */
int Sequencer::check_timestamp_(TimeStamp ts, Lock& lock) {
    int error_code = AKU_SUCCESS;
    if (ts < top_timestamp_) {
        auto delta = top_timestamp_ - ts;
        if (delta.value > window_size_.value) {
            error_code = AKU_ELATE_WRITE;
        }
        return error_code;
    }
    auto point = get_checkpoint_(ts);
    if (point > checkpoint_) {
        // Create new checkpoint
        make_checkpoint_(point, lock);
        if (!lock.owns_lock()) {
            // Previous checkpoint not completed
            error_code = AKU_EBUSY;
        }
    }
    top_timestamp_ = ts;
    return error_code;
}

std::tuple<int, Sequencer::Lock> Sequencer::add(TimeSeriesValue const& value) {
    // FIXME: max_cache_size_ is not used
    int status;
    Lock lock(progress_flag_, defer_lock);
    status = check_timestamp_(get<0>(value.key_), lock);
    if (status != AKU_SUCCESS) {
        return make_tuple(status, move(lock));
    }
    key_.pop_back();
    key_.push_back(value);
    auto begin = runs_.begin();
    auto end = runs_.end();
    auto insert_it = lower_bound(begin, end, key_, top_element_more<SortedRun>);
    if (insert_it == runs_.end()) {
        SortedRun new_pile;
        new_pile.push_back(value);
        runs_.push_back(new_pile);
    } else {
        int run_ix = distance(begin, insert_it);
        lock_run(run_ix);
        insert_it->push_back(value);
        unlock_run(run_ix);
    }
    return make_tuple(AKU_SUCCESS, move(lock));
}

Sequencer::Lock Sequencer::close() {
    Lock lock(progress_flag_, defer_lock);
    if (!lock.try_lock()) {
        return move(lock);
    }
    if (!ready_.empty()) {
        throw runtime_error("sequencer invariant is broken");
    }
    lock_all_runs();
    for (auto& sorted_run: runs_) {
        ready_.push_back(move(sorted_run));
    }
    unlock_all_runs();
    runs_.clear();
    atomic_thread_fence(memory_order_acq_rel);
    return move(lock);
}

template<class TKey, int dir>
struct MergePred;

template<class TKey>
struct MergePred<TKey, AKU_CURSOR_DIR_FORWARD> {
    greater<TKey> greater_;
    bool operator () (TKey const& lhs, TKey const& rhs) const {
        return greater_(lhs, rhs);
    }
};

template<class TKey>
struct MergePred<TKey, AKU_CURSOR_DIR_BACKWARD> {
    less<TKey> less_;
    bool operator () (TKey const& lhs, TKey const& rhs) const {
        return less_(lhs, rhs);
    }
};

template<class TRun, int dir>
struct RunIter;

template<class TRun>
struct RunIter<TRun, AKU_CURSOR_DIR_FORWARD> {
    typedef typename TRun::const_iterator iterator;
    static iterator begin(TRun const& run) {
        return run.begin();
    }
    static iterator end(TRun const& run) {
        return run.end();
    }
};

template<class TRun>
struct RunIter<TRun, AKU_CURSOR_DIR_BACKWARD> {
    typedef typename TRun::const_reverse_iterator iterator;
    static iterator begin(TRun const& run) {
        return run.rbegin();
    }
    static iterator end(TRun const& run) {
        return run.rend();
    }
};

template <int dir, class TRun>
void kway_merge(vector<TRun> const& runs, Caller& caller, InternalCursor* out_iter, PageHeader const* page) {
    size_t n = runs.size();
    typedef typename RunIter<TRun, dir>::iterator iter_t;
    typedef typename TRun::value_type value_t;
    iter_t iter[n], ends[n];
    int cnt = 0;
    for(auto i = runs.begin(); i != runs.end(); i++) {
        iter[cnt] = RunIter<TRun, dir>::begin(*i);
        ends[cnt] = RunIter<TRun, dir>::end(*i);
        cnt++;
    }

    typedef tuple<value_t, int> HeapItem;
    typedef MergePred<HeapItem, dir> Comp;
    typedef boost::heap::skew_heap<HeapItem, boost::heap::compare<Comp>> Heap;
    Heap heap;

    for(auto index = 0u; index < n; index++) {
        if (iter[index] != ends[index]) {
            auto value = *iter[index];
            iter[index]++;
            heap.push(make_tuple(value, index));
        }
    }

    while(!heap.empty()) {
        auto item = heap.top();
        auto point = get<0>(item);
        int index = get<1>(item);
        out_iter->put(caller, point.value, page);
        heap.pop();
        if (iter[index] != ends[index]) {
            auto point = *iter[index];
            iter[index]++;
            heap.push(make_tuple(point, index));
        }
    }
}

void Sequencer::merge(Caller& caller, InternalCursor* cur, Lock&& lock) {
    bool owns_lock = lock.owns_lock();
    if (!owns_lock) {
        // Error! Merge called too early
        cur->set_error(caller, AKU_EBUSY);
        return;
    }

    if (ready_.size() == 0) {
        // Things go crazy
        cur->set_error(caller, AKU_ENO_DATA);
        return;
    }

    kway_merge<AKU_CURSOR_DIR_FORWARD>(ready_, caller, cur, page_);

    // Sequencer invariant - if progress_flag_ is unset - ready_ flag must be empty
    // we've got only one place to store ready to sync data, if such data is present
    // progress_flag_ must be set (it indicates that merge/sync procedure is in progress)
    // after that we must clear ready_ collection and free some space for new data, after
    // that progress_flag_ can be cleared.

    ready_.clear();
    atomic_thread_fence(memory_order_acq_rel);
    cur->complete(caller);
}

void Sequencer::lock_run(int ix) const {
    int busy_wait_counter = RUN_LOCK_BUSY_COUNT;
    int backoff_len = 0;
    while(true) {
        bool is_busy = run_lock_flags_[RUN_LOCK_FLAGS_MASK & ix].test_and_set();
        if (!is_busy) {
            return;
        } else {
            // Busy wait
            if (busy_wait_counter) {
                busy_wait_counter--;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_len));
                if (backoff_len < RUN_LOCK_MAX_BACKOFF) {
                    backoff_len++;
                }
            }
        }
    }
}

void Sequencer::unlock_run(int ix) const {
    run_lock_flags_[RUN_LOCK_FLAGS_MASK & ix].clear();
}

void Sequencer::lock_all_runs() const {
   for (int i = 0; i < RUN_LOCK_FLAGS_SIZE; i++) {
       lock_run(i);
   }
}

void Sequencer::unlock_all_runs() const {
   for (int i = 0; i < RUN_LOCK_FLAGS_SIZE; i++) {
       unlock_run(i);
   }
}

struct SearchPredicate {
    SearchQuery const& query;
    SearchPredicate(SearchQuery const& q) : query(q) {}

    bool operator () (TimeSeriesValue const& value) const {
        if (query.lowerbound < get<TIMESTMP>(value.key_) &&
            query.upperbound > get<TIMESTMP>(value.key_))
        {
            if (query.param_pred(get<PARAM_ID>(value.key_)) == SearchQuery::MATCH) {
                return true;
            }
        }
        return false;
    }
};

void Sequencer::filter(SortedRun const& run, SearchQuery const& q, std::vector<SortedRun>* results) const {
    // TODO: use more effective algorithm, based on binary search
    SortedRun result;
    copy_if(run.begin(), run.end(), std::back_inserter(result), SearchPredicate(q));
    results->push_back(move(result));
}

void Sequencer::search(Caller& caller, InternalCursor* cur, const SearchQuery &query) const {
    std::lock_guard<std::mutex> guard(progress_flag_);
    // we can get here only before checkpoint (or after merge was completed)
    // that means that ready_ is empty
    assert(ready_.empty());
    std::vector<SortedRun> filtered;
    int run_ix = 0;
    for (const auto& run: runs_) {
        lock_run(run_ix);
        filter(run, query, &filtered);
        unlock_run(run_ix);
        run_ix++;
    }
    if (query.direction == AKU_CURSOR_DIR_FORWARD) {
        kway_merge<AKU_CURSOR_DIR_FORWARD>(filtered, caller, cur, page_);
    } else {
        kway_merge<AKU_CURSOR_DIR_BACKWARD>(filtered, caller, cur, page_);
    }
}
}  // namespace Akumuli
