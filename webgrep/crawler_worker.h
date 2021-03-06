#ifndef CRAWLER_WORKER_H
#define CRAWLER_WORKER_H

#include <memory>
#include <functional>
#include <thread>
#include <condition_variable>
#include "client_http.hpp"
#include <atomic>
#include <array>
#include "noncopyable.hpp"
#include "linked_task.h"

#define CRAWLER_WORKER_USE_REGEXP 0

namespace WebGrep {


struct LonelyTask;

typedef std::function<void()> CallableFunc_t;
typedef std::function<void(std::shared_ptr<LinkedTask> rootNode, LinkedTask* node)> NodeScanCallback_t;

//---------------------------------------------------------------
/** The structure must be copyable in such manner that
 * the original and copied resources can be used concurrently in different threads.
 * It is intended for use as temporary storage that has callback functors,
 * callback functors contain ref. counted pointers to some external entities,
 * so triggering them will make some changes, modification of WorkerCtx
 * will not go out of scope { } where it is operated on.
*/
struct WorkerCtx
{
  WorkerCtx()
  {
#if CRAWLER_WORKER_USE_REGEXP
    urlGrepExpressions.push_back(std::regex("href[ ]?=[ ]?\"[http]?[https]?[:/]?/[^ \"]*\"", std::regex::egrep));
#endif
    scheme.fill(0);
    data_ = nullptr;
  }

  //--------------------------------------------------------
  WebGrep::Client httpClient;
  WebGrep::Scheme6 scheme;// must be "http\0\0" or "https\0"
  std::string hostPort; // site.com:443

  std::shared_ptr<LinkedTask> rootNode;

  /** An array of expression to grep URLs from the web pages.*/
#if CRAWLER_WORKER_USE_REGEXP
  std::vector<std::regex> urlGrepExpressions; //sometimes we do it without regexp.
#endif

  void* data_;
  //--------------------------------------------------------

  std::function<void(const std::string& )> onException;

  /** Called by functions to schedule FuncDownloadGrepRecursive.
   * The pointer is not used, it's object is copied */
  std::function<void(const LonelyTask*)> scheduleTask;

  /** Call this one to schedule any task:*/
  std::function<void(CallableDoubleFunc)> scheduleFunctor;

  /** This is a hack to work with a thread handle to serialize sequential
   *  functors to one thread. Be careful with the data.*/
  std::function<WebGrep::TPool_ThreadDataPtr()> getThreadHandle;

  //---- callbacks, all three are set by the working functions -------

  /** when max. links count reached.*/
  WebGrep::NodeScanCallback_t onMaximumLinksCount;

  /** Invoked on each page parsed.*/
  WebGrep::NodeScanCallback_t pageMatchFinishedCb;
  WebGrep::NodeScanCallback_t nodeListFinishedCb;

  /** Invoked when a new level of child nodes has spawned, e.g. a sequence of pages to be parsed. */
  WebGrep::NodeScanCallback_t childLevelSpawned;

  //some utilities as methods:
  typedef bool (*WorkFunc_t)(LinkedTask* task, WorkerCtx& w);

  /** schedule all all nodes of the branch(by .next item) to be executed by given method.
   * @param skipCount: how much branch nodes to skip.
   * @param spray: TRUE when the tasks are assigned to different threads, FALSE makes them queued to one thread.
   * @return number of items scheduled. */
  size_t scheduleBranchExec(LinkedTask* node, WorkFunc_t method, uint32_t skipCount = 0, bool spray = true);

  /** schedule all all nodes of the branch(by .next item) to be executed by given functor.
   * @return number of items scheduled. */
  size_t scheduleBranchExecFunctor(LinkedTask* task, std::function<void(LinkedTask*)> functor,
                                  uint32_t skipCount = 0);

};
//---------------------------------------------------------------
struct LonelyTask
{
  LonelyTask();

  std::shared_ptr<LinkedTask> root;
  LinkedTask* target;
  bool (*action)(LinkedTask*, WorkerCtx&);
  WorkerCtx ctx;
  void* additional;//caller cares
};

//---------------------------------------------------------------
/** Downloads the page content and stores it in (GrepVars)task->grepVars
 *  variable, (volatile bool)task->grepVars.pageIsReady will be set to TRUE
 *  on successfull download; the page content will be stored in task->grepVars.pageContent
*/
bool FuncDownloadOne(LinkedTask* task, WorkerCtx& w);

/** Calls FuncDownloadOne(task, w) if FALSE == (volatile bool)task->grepVars.pageIsReady,
 *  then if the download is successfull it'll grep the http:// and href= links from the page,
 *  the results are stored in a container of type (vector<pair<string::const_iterator,string::const_iterator>>)
 *  at variable (task->grepVars.matchURLVector and task->grepVars.matchTextVector)
 *  where each pair of const iterators points to the begin and the end of matched strings
 *  in task->grepVars.pageContent, the iterators are valid until grepVars.pageContent exists.
*/
bool FuncGrepOne(LinkedTask* task, WorkerCtx& w);

/** Call FuncDownloadOne(task,w) multiple times: once for each new http:// URL
 *  in a page's content. It won't use recursion, but will utilize appropriate
 *  callbacks to put new tasks as functors in a multithreaded work queue.
 *
 *  What it does with the list (LinkedTask*) task?
 *  It spawns .child item assigning it (level + 1), it'll be head of a new list
 *  that will contain parse results for the task's page and other subtasks,
 *  appearance of each subtask is caused by finding a URL that we can grep
 *  until counter's limit is reached.
*/
bool FuncDownloadGrepRecursive(LinkedTask* task, WorkerCtx& w);
//---------------------------------------------------------------


}//WebGrep

#endif // CRAWLER_WORKER_H
