/** Galois scheduler and runtime -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Implementation of the Galois foreach iterator. Includes various 
 * specializations to operators to reduce runtime overhead.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#ifndef GALOIS_RUNTIME_EXECUTOR_FOREACH_H
#define GALOIS_RUNTIME_EXECUTOR_FOREACH_H

#include "Galois/gtuple.h"
#include "Galois/Mem.h"
#include "Galois/Statistic.h"
#include "Galois/Threads.h"
#include "Galois/Traits.h"
#include "Galois/Runtime/Barrier.h"
#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/ForEachTraits.h"
#include "Galois/Runtime/Range.h"
#include "Galois/Runtime/Support.h"
#include "Galois/Runtime/Termination.h"
#include "Galois/Runtime/ThreadPool.h"
#include "Galois/Runtime/UserContextAccess.h"
#include "Galois/WorkList/Chunked.h"
#include "Galois/WorkList/GFifo.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace Galois {
//! Internal Galois functionality - Use at your own risk.
namespace Runtime {

static constexpr unsigned GALOIS_DEFAULT_CHUNK_SIZE = 32;
typedef WorkList::dChunkedFIFO<GALOIS_DEFAULT_CHUNK_SIZE> defaultWL;

template<typename value_type>
class AbortHandler {
  struct Item { value_type val; int retries; };

  typedef WorkList::GFIFO<Item> AbortedList;
  PerThreadStorage<AbortedList> queues;
  bool useBasicPolicy;
  
  /**
   * Policy: serialize via tree over packages.
   */
  void basicPolicy(const Item& item) {
    unsigned tid = LL::getTID();
    unsigned package = LL::getPackageForSelf(tid);
    queues.getRemote(LL::getLeaderForPackage(package / 2))->push(item);
  }

  /**
   * Policy: retry work 2X locally, then serialize via tree on package (trying
   * twice at each level), then serialize via tree over packages.
   */
  void doublePolicy(const Item& item) {
    int retries = item.retries - 1;
    if ((retries & 1) == 1) {
      queues.getLocal()->push(item);
      return;
    } 
    
    unsigned tid = LL::getTID();
    unsigned package = LL::getPackageForSelf(tid);
    unsigned leader = LL::getLeaderForPackage(package);
    if (tid != leader) {
      unsigned next = leader + (tid - leader) / 2;
      queues.getRemote(next)->push(item);
    } else {
      queues.getRemote(LL::getLeaderForPackage(package / 2))->push(item);
    }
  }

  /**
   * Policy: retry work 2X locally, then serialize via tree on package but
   * try at most 3 levels, then serialize via tree over packages.
   */
  void boundedPolicy(const Item& item) {
    int retries = item.retries - 1;
    if (retries < 2) {
      queues.getLocal()->push(item);
      return;
    } 
    
    unsigned tid = LL::getTID();
    unsigned package = LL::getPackageForSelf(tid);
    unsigned leader = LL::getLeaderForPackage(package);
    if (retries < 5 && tid != leader) {
      unsigned next = leader + (tid - leader) / 2;
      queues.getRemote(next)->push(item);
    } else {
      queues.getRemote(LL::getLeaderForPackage(package / 2))->push(item);
    }
  }

  /**
   * Retry locally only.
   */
  void eagerPolicy(const Item& item) {
    queues.getLocal()->push(item);
  }

public:
  AbortHandler() {
    // XXX(ddn): Implement smarter adaptive policy
    useBasicPolicy = LL::getMaxPackages() > 2;
  }

  value_type& value(Item& item) const { return item.val; }
  value_type& value(value_type& val) const { return val; }

  void push(const value_type& val) {
    Item item = { val, 1 };
    queues.getLocal()->push(item);
  }

  void push(const Item& item) {
    Item newitem = { item.val, item.retries + 1 };
    if (useBasicPolicy)
      basicPolicy(newitem);
    else
      doublePolicy(newitem);
  }

  AbortedList* getQueue() { return queues.getLocal(); }
};

//TODO(ddn): Implement wrapper to allow calling without UserContext
//TODO(ddn): Check for operators that implement both with and without context
template<class WorkListTy, class FunctionTy, class ArgsTy>
class ForEachExecutor {
public:
  static const bool needsStats = !exists_by_supertype<does_not_need_stats_tag, ArgsTy>::value;
  static const bool needsPush = !exists_by_supertype<does_not_need_push_tag, ArgsTy>::value;
  static const bool needsAborts = !exists_by_supertype<does_not_need_aborts_tag, ArgsTy>::value;
  static const bool needsPia = exists_by_supertype<needs_per_iter_alloc_tag, ArgsTy>::value;
  static const bool needsBreak = exists_by_supertype<needs_parallel_break_tag, ArgsTy>::value;

protected:
  typedef typename WorkListTy::value_type value_type; 

  struct ThreadLocalData {
    FunctionTy function;
    UserContextAccess<value_type> facing;
    SimpleRuntimeContext ctx;
    unsigned long stat_conflicts;
    unsigned long stat_iterations;
    unsigned long stat_pushes;
    const char* loopname;
    ThreadLocalData(const FunctionTy& fn, const char* ln)
      : function(fn), stat_conflicts(0), stat_iterations(0), stat_pushes(0), 
        loopname(ln)
    {}
    ~ThreadLocalData() {
      if (needsStats) {
        reportStat(loopname, "Conflicts", stat_conflicts);
        reportStat(loopname, "Commits", stat_iterations - stat_conflicts);
        reportStat(loopname, "Pushes", stat_pushes);
        reportStat(loopname, "Iterations", stat_iterations);
      }
    }
  };

  // NB: Place dynamically growing wl after fixed-size PerThreadStorage
  // members to give higher likelihood of reclaiming PerThreadStorage

  AbortHandler<value_type> aborted; 
  TerminationDetection& term;

  WorkListTy wl;
  FunctionTy origFunction;
  const char* loopname;
  bool broke;

  inline void commitIteration(ThreadLocalData& tld) {
    if (needsPush) {
      //auto ii = tld.facing.getPushBuffer().begin();
      //auto ee = tld.facing.getPushBuffer().end();
      auto& pb = tld.facing.getPushBuffer();
      auto n = pb.size();
      if (n) {
	tld.stat_pushes += n;
        wl.push(pb.begin(), pb.end());
        pb.clear();
      }
    }
    if (needsPia)
      tld.facing.resetAlloc();
    if (needsAborts)
      tld.ctx.commitIteration();
    //++tld.stat_commits;
  }

  template<typename Item>
  GALOIS_ATTRIBUTE_NOINLINE
  void abortIteration(const Item& item, ThreadLocalData& tld) {
    assert(needsAborts);
    tld.ctx.cancelIteration();
    ++tld.stat_conflicts;
    aborted.push(item);
    //clear push buffer
    if (needsPush)
      tld.facing.resetPushBuffer();
    //reset allocator
    if (needsPia)
      tld.facing.resetAlloc();
  }

  inline void doProcess(value_type& val, ThreadLocalData& tld) {
    if (needsAborts)
      tld.ctx.startIteration();
    ++tld.stat_iterations;
    tld.function(val, tld.facing.data());
    commitIteration(tld);
  }

  void runQueueSimple(ThreadLocalData& tld) {
    Galois::optional<value_type> p;
    while ((p = wl.pop())) {
      doProcess(*p, tld);
    }
  }

  template<int limit, typename WL>
  void runQueue(ThreadLocalData& tld, WL& lwl) {
    Galois::optional<typename WL::value_type> p;
    int num = 0;
#ifdef GALOIS_USE_LONGJMP
    if (setjmp(hackjmp) == 0) {
      while ((!limit || num++ < limit) && (p = lwl.pop())) {
	doProcess(aborted.value(*p), tld);
      }
    } else {
      clearReleasable();
      clearConflictLock();
      abortIteration(*p, tld);
    }
#else
    try {
      while ((!limit || num++ < limit) && (p = lwl.pop())) {
	doProcess(aborted.value(*p), tld);
      }
    } catch (ConflictFlag const& flag) {
      abortIteration(*p, tld);
    }
#endif
  }

  GALOIS_ATTRIBUTE_NOINLINE
  void handleAborts(ThreadLocalData& tld) {
    runQueue<0>(tld, *aborted.getQueue());
  }

  void fastPushBack(typename UserContextAccess<value_type>::PushBufferTy& x) {
    wl.push(x.begin(), x.end());
    x.clear();
  }

  bool checkEmpty(WorkListTy&, ThreadLocalData&, ...) { return true; }

  template<typename WL>
  auto checkEmpty(WL& wl, ThreadLocalData& tld, int) -> decltype(wl.empty(), bool()) {
    return wl.empty();
  }

  template<bool couldAbort, bool isLeader>
  void go() {
    // Thread-local data goes on the local stack to be NUMA friendly
    ThreadLocalData tld(origFunction, loopname);
    if (needsBreak)
      tld.facing.setBreakFlag(&broke);
    if (couldAbort)
      setThreadContext(&tld.ctx);
    if (needsPush && !couldAbort)
      tld.facing.setFastPushBack(
          std::bind(&ForEachExecutor::fastPushBack, this, std::placeholders::_1));
    unsigned long old_iterations = 0;
    while (true) {
      do {
        // Run some iterations
        if (couldAbort || needsBreak) {
          constexpr int __NUM = (needsBreak || isLeader) ? 64 : 0;
          runQueue<__NUM>(tld, wl);
          // Check for abort
          if (couldAbort)
            handleAborts(tld);
        } else { // No try/catch
          runQueueSimple(tld);
        }

        bool didWork = old_iterations != tld.stat_iterations;
        old_iterations = tld.stat_iterations;

        // Update node color and prop token
        term.localTermination(didWork);
        LL::asmPause(); // Let token propagate
      } while (!term.globalTermination() && (!needsBreak || !broke));

      if (checkEmpty(wl, tld, 0))
        break;
      if (needsBreak && broke)
        break;
      term.initializeThread();
      getSystemBarrier().wait();
    }

    if (couldAbort)
      setThreadContext(0);
  }

  template<typename... WArgsTy>
  ForEachExecutor(const FunctionTy& f, const ArgsTy& args, int, const WArgsTy&... wargs):
    term(getSystemTermination()),
    wl(wargs...),
    origFunction(f),
    loopname(get_by_supertype<loopname_tag>(args).value),
    broke(false) { }

  ForEachExecutor(const FunctionTy& f, const ArgsTy& args, int, int_seq<>):
    ForEachExecutor(f, args, 0) {}

  template<int... Is>
  ForEachExecutor(const FunctionTy& f, const ArgsTy& args, int, int_seq<Is...>):
    ForEachExecutor(f, args, 0, std::get<Is...>(get_by_supertype<wl_tag>(args).args)) {}

public:
  ForEachExecutor(const FunctionTy& f, const ArgsTy& args):
    ForEachExecutor(f, args, 0, typename make_int_seq<std::tuple_size<decltype(get_by_supertype<wl_tag>(args).args)>::value>::type{}) {}
  
  template<typename RangeTy>
  void init(const RangeTy&) { }

  template<typename RangeTy>
  void initThread(const RangeTy& range) {
    wl.push_initial(range);
    term.initializeThread();
  }

  void operator()() {
    bool isLeader = LL::isPackageLeaderForSelf(LL::getTID());
    bool couldAbort = needsAborts && activeThreads > 1;
    if (couldAbort && isLeader)
      go<true, true>();
    else if (couldAbort && !isLeader)
      go<true, false>();
    else if (!couldAbort && isLeader)
      go<false, true>();
    else
      go<false, false>();
  }
};

template<typename WLTy>
constexpr auto has_with_iterator(int)
  -> decltype(std::declval<typename WLTy::template with_iterator<int*>::type>(), bool()) 
{
  return true;
}

template<typename>
constexpr auto has_with_iterator(...) -> bool {
  return false;
}

template<typename WLTy, typename IterTy, typename Enable = void>
struct reiterator {
  typedef WLTy type;
};

template<typename WLTy, typename IterTy>
struct reiterator<WLTy, IterTy,
  typename std::enable_if<has_with_iterator<WLTy>(0)>::type> 
{
  typedef typename WLTy::template with_iterator<IterTy>::type type;
};

template<typename Fn, typename T>
constexpr auto takes_context(int) 
  -> decltype(std::declval<typename std::result_of<Fn(T&, UserContext<T>&)>::type>(), bool())
{
  return true;
}

template<typename Fn, typename T>
constexpr auto takes_context(...) -> bool
{
  return false;
}

template<typename Fn, typename T, typename Enable = void>
struct MakeTakeContext
{
  Fn fn;

  void operator()(T& item, UserContext<T>& ctx) const {
    fn(item);
  }
};

template<typename Fn, typename T>
struct MakeTakeContext<Fn, T, typename std::enable_if<takes_context<Fn, T>(0)>::type>
{
  Fn fn;

  void operator()(T& item, UserContext<T>& ctx) const {
    fn(item, ctx);
  }
};

template<typename WorkListTy, typename T, typename RangeTy, typename FunctionTy, typename ArgsTy>
auto for_each_impl_(const RangeTy& range, const FunctionTy& fn, const ArgsTy& args) 
  -> typename std::enable_if<takes_context<FunctionTy, T>(0)>::type
{
  typedef ForEachExecutor<WorkListTy, FunctionTy, ArgsTy> WorkTy;
  Barrier& barrier = getSystemBarrier();
  WorkTy W(fn, args);
  W.init(range);
  getSystemThreadPool().run(activeThreads,
                            [&W, &range]() { W.initThread(range); },
                            std::ref(barrier),
                            std::ref(W));
}

template<typename WorkListTy, typename T, typename RangeTy, typename FunctionTy, typename ArgsTy>
auto for_each_impl_(const RangeTy& range, const FunctionTy& fn, const ArgsTy& args) 
  -> typename std::enable_if<!takes_context<FunctionTy, T>(0)>::type
{
  typedef MakeTakeContext<FunctionTy, T> WrappedFunction;
  auto newArgs = std::tuple_cat(args,
      get_default_trait_values(args,
        std::make_tuple(does_not_need_push_tag {}),
        std::make_tuple(does_not_need_push<> {})));
  typedef ForEachExecutor<WorkListTy, WrappedFunction, decltype(newArgs)> WorkTy;
  Barrier& barrier = getSystemBarrier();
  WorkTy W(WrappedFunction {fn}, newArgs);
  W.init(range);
  getSystemThreadPool().run(activeThreads,
                            [&W, &range]() { W.initThread(range); },
                            std::ref(barrier),
                            std::ref(W));
}

//TODO(ddn): Think about folding in range into args too
template<typename RangeTy, typename FunctionTy, typename ArgsTy>
void for_each_impl(const RangeTy& range, const FunctionTy& fn, const ArgsTy& args) {
  typedef typename std::iterator_traits<typename RangeTy::iterator>::value_type value_type; 
  typedef typename get_type_by_supertype<wl_tag, ArgsTy>::type::type BaseWorkListTy;
  typedef typename reiterator<BaseWorkListTy, typename RangeTy::iterator>::type
    ::template retype<value_type>::type WorkListTy;

  for_each_impl_<WorkListTy, value_type>(range, fn, args);
}

//! Normalize arguments to for_each
template<typename RangeTy, typename FunctionTy, typename TupleTy>
void for_each_gen(const RangeTy& r, const FunctionTy& fn, const TupleTy& tpl) {
  static_assert(!exists_by_supertype<char*, TupleTy>::value, "old loopname");
  static_assert(!exists_by_supertype<char const *, TupleTy>::value, "old loopname");
  static_assert(!exists_by_supertype<bool, TupleTy>::value, "old steal");

  static const bool forceNew = false;
  static_assert(!forceNew || Runtime::DEPRECATED::ForEachTraits<FunctionTy>::NeedsAborts, "old type trait");
  static_assert(!forceNew || Runtime::DEPRECATED::ForEachTraits<FunctionTy>::NeedsStats, "old type trait");
  static_assert(!forceNew || Runtime::DEPRECATED::ForEachTraits<FunctionTy>::NeedsPush, "old type trait");
  static_assert(!forceNew || !Runtime::DEPRECATED::ForEachTraits<FunctionTy>::NeedsBreak, "old type trait");
  static_assert(!forceNew || !Runtime::DEPRECATED::ForEachTraits<FunctionTy>::NeedsPIA, "old type trait");
  if (forceNew) {
    Runtime::for_each_impl(r, fn,
        std::tuple_cat(tpl,
          typename function_traits<FunctionTy>::type {}, 
          get_default_trait_values(tpl,
            std::make_tuple(loopname_tag {}, wl_tag {}),
            std::make_tuple(loopname {}, wl<defaultWL>()))));
  } else {
    auto tags = typename DEPRECATED::ExtractForEachTraits<FunctionTy>::tags_type {};
    auto values = typename DEPRECATED::ExtractForEachTraits<FunctionTy>::values_type {};
    auto ttpl = get_default_trait_values(tpl, tags, values);
    auto dtpl = std::tuple_cat(tpl, ttpl);
    Runtime::for_each_impl(r, fn,
        std::tuple_cat(dtpl,
          typename function_traits<FunctionTy>::type {}, 
          get_default_trait_values(dtpl,
            std::make_tuple(loopname_tag {}, wl_tag {}),
            std::make_tuple(loopname {}, wl<defaultWL>()))));
  }
}

} // end namespace Runtime
} // end namespace Galois
#endif