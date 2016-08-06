#include "crawler.h"
#include <iostream>
#include "crawler_worker.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include "boost/lockfree/queue.hpp"
#include "boost/thread/thread_pool.hpp"

namespace WebGrep {

class Crawler::CrawlerPV
{
public:
  CrawlerPV()
  {
    firstPageTask = nullptr;
    maxLinksCount.store(4096);
    currentLinksCount.store(0);

    onMainSubtaskCompleted = [this](LinkedTask* task, WorkerCtxPtr)
    {
      std::lock_guard<std::mutex> lk(wlistMutex); (void)lk;
      std::cout << "subtask completed: " << task->grepVars.targetUrl << "\n";
      std::cout << "subtask content: \n" << task->grepVars.pageContent << "\n";
    };
  }

  virtual ~CrawlerPV()
  {
    try {
      clear();
    } catch(...)
    { }
  }

  void clear()
  {
    std::lock_guard<std::mutex> lk(wlistMutex); (void)lk;
    stop(false/*no lock*/);
    if (nullptr != workersPool)
      {
        workersPool->close();
        workersPool->join();
      }
    WebGrep::DeleteList(firstPageTask);
    firstPageTask = nullptr;
    currentLinksCount.store(0);
  }

  void stop(bool withMLock)
  {
    std::shared_ptr<std::lock_guard<std::mutex>> lk;
    if(withMLock) {
        lk = std::make_shared<std::lock_guard<std::mutex>> (wlistMutex);
      }
    for(WorkerCtxPtr& ctx : workContexts)
      {
        ctx->running = false;
      }
  }

  LinkedTask* firstPageTask;

  std::mutex wlistMutex;
  std::unique_ptr<boost::executors::basic_thread_pool> workersPool;
  std::vector<WorkerCtxPtr> workContexts;
  std::atomic_uint maxLinksCount, currentLinksCount;

  std::function<void(LinkedTask*, WorkerCtxPtr)> onMainSubtaskCompleted;
};
//---------------------------------------------------------------
Crawler::Crawler()
{
  pv.reset(new CrawlerPV);
  onException = [] (const std::string& what){ std::cerr << what << "\n"; };
}
void Crawler::clear()
{
  pv->clear();
}
//---------------------------------------------------------------
bool Crawler::start(const std::string& url,
                    const std::string& grepRegex,
                    unsigned maxLinks, unsigned threadsNum)
{

  LinkedTask*& root(pv->firstPageTask);

  try {
    stop();
    setMaxLinks(maxLinks);
    setThreadsNumber(threadsNum);
    std::lock_guard<std::mutex> lk(pv->wlistMutex); (void)lk;
    if (nullptr == root || root->grepVars.targetUrl != url)
      {
        WebGrep::DeleteList(pv->firstPageTask);
        pv->firstPageTask = nullptr;
        //alloc toplevel node:
        root = new LinkedTask;
        root->linksCounterPtr = &(pv->currentLinksCount);
        root->maxLinksCountPtr = &(pv->maxLinksCount);
      }

    GrepVars* g = &(root->grepVars);
    g->targetUrl = url;
    g->grepExpr = grepRegex;
    for(WorkerCtxPtr& ctx : pv->workContexts)
      {//enable workers to spawn subtasks e.g. "start"
        ctx->running = true;
      }
  } catch(std::exception& e)
  {
    std::cerr << e.what() << std::endl;
    this->onException(e.what());
    return false;
  }

  //just picking up first worker context
  auto worker = pv->workContexts[0];

  //submit a root-task: get the first page and then follow it's content's links in new threads
  auto fn = [this, worker]() {
      //this functor will wake-up pending task from another thread
      FuncGrepOne(pv->firstPageTask, worker);
      pv->firstPageTask->spawnGreppedSubtasks(worker->hostPort);
      std::string temp;
      GrepVars& g(pv->firstPageTask->grepVars);
      for(size_t cnt = 0; cnt < g.matchURLVector.size(); ++cnt)
        {
          temp.assign(g.matchURLVector[cnt].first, g.matchURLVector[cnt].second);
          std::cerr << temp << std::endl;
        }

      //ventillate subtasks:
      std::lock_guard<std::mutex> lk(pv->wlistMutex); (void)lk;
      size_t item = 0;

      //rearrange first task's spawned subitems to different independent contexts:
      for(auto node = WebGrep::ItemLoadAcquire(pv->firstPageTask->child);
          nullptr != node;
          ++item %= pv->workContexts.size(), node = WebGrep::ItemLoadAcquire(node->next) )
        {
          auto ctx = pv->workContexts[item];
          //submit recursive grep for each 1-st level subtask:
          pv->workersPool->submit([node, ctx]
                                  (){ FuncDownloadGrepRecursive(node, ctx); });
        }
    };
  try {
    pv->workersPool->submit(fn);
  } catch(const std::exception& ex)
  {
    if(onException)
      onException(ex.what());
    return false;
  }
  return true;
}

void Crawler::stop()
{
  pv->stop(true/*with lock*/);
}

void Crawler::setMaxLinks(unsigned maxScanLinks)
{
  //sync with load(acquire)
  pv->maxLinksCount.store(maxScanLinks);
}
//---------------------------------------------------------------
void Crawler::setThreadsNumber(unsigned nthreads)
{
  if (0 == nthreads)
    {
      std::cerr << "void Crawler::setThreadsNumber(unsigned nthreads) -> 0 value ignored.\n";
      return;
    }
  try {
    std::lock_guard<std::mutex> lk(pv->wlistMutex); (void)lk;
    pv->stop(false/*mutex lock*/);
    if (nullptr != pv->workersPool)
      {
        pv->workersPool->close();
        pv->workersPool->join();
      }

    pv->workContexts.resize(nthreads);
    for(WorkerCtxPtr& wp : pv->workContexts)
      {
        if (nullptr == wp)
          wp = std::make_shared<WorkerCtx>();
      }

    pv->workersPool.reset(new boost::executors::basic_thread_pool(nthreads));
  } catch(const std::exception& ex)
  {
    std::cerr << ex.what() << std::endl;
    if(onException) {
      onException(ex.what());
    }
  }
 }
//---------------------------------------------------------------

}//WebGrep
