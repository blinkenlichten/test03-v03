#ifndef LINKEDTASK_H
#define LINKEDTASK_H
#include <string>
#include <regex>
#include "noncopyable.hpp"
#include <atomic>

#include <functional>
#include <iostream>
#include "thread_pool.h"

namespace WebGrep {

const size_t MaxURLlen = 8192;

class WorkerCtx;
//---------------------------------------------------------------
/** Contains match results -- an iterators pointing to .pageContent.*/
struct GrepVars
{
  GrepVars() : responseCode(0), pageIsReady(false), pageIsParsed(false)
  {
    scheme.fill(0);
  }

  std::array<char, 6> scheme;// must be set to "http\0\0" or "https\0"
  std::string targetUrl;

  std::regex grepExpr;  //< regexp to be matched
  long responseCode;       //< last HTTP GET response code

  std::string pageContent;//< html content

  //contains matched URLs in .pageContent
  typedef std::string::const_iterator COT_t;
  typedef std::pair<COT_t,COT_t> CIteratorPair;

  /** After (TRUE == pageIsParsed) matchURLVector will contain const_iterator
   *  that point to a matched string in this->pageContent;
   *  similarly matchTextVector will get iterators pointing to this->pageContent
   *  where text search conditions has met.
 */
  std::vector<CIteratorPair> matchURLVector, matchTextVector;

  //must be set to true when it's safe to access .pageContent from other threads:
  volatile bool pageIsReady;
  volatile bool pageIsParsed;
};
//---------------------------------------------------------------
class LinkedTask;

// Recursively traverse the list and call a function each item
void TraverseFunc(LinkedTask* head, void* additional,
                  void(*func)(LinkedTask*, void*));

// Recursively traverse the list and call functor on each item
void TraverseFunctor(LinkedTask* head, void* additional,
                     std::function<void(LinkedTask*, void* additional)>);

// Free memory recursively. NOT THREAD SAFE! Must be syncronized.
void DeleteList(LinkedTask* head);

/** Apply functor for each item on same branch accessed by(head->next).
 * Any exceptions from the functor will be catched and printed to std::cerr.
 * @return how much times functor has been invoked.
 */
size_t ForEachOnBranch(LinkedTask* head, std::function<void(LinkedTask*)> functor, uint32_t skipCount = 0);
//---------------------------------------------------------------


/** LinkedTask : a tree list that is using atomic pointers
 *  for child nodes (to be able to read without locking).
 *  Once the node is constructed -- it is okay to read tree pointers concurrently and modify them,
 *  the variable (GrapVars grepVars) must be carefully accessed on callbacks
 *  when in event-driven flow,
 *  (volatile bool)grepVars.pageIsReady indicates that grepVars.pageContent is constructed;
 *  (volatile bool)grepVars.pageIsParsed indicates that grepVars.pageContent is already parsed
 *  and grepVars.matchURLVector is filled;
 *  Instead of prasing the task again on current item,
 *  better construct new node and move the data there safely.
 */
class LinkedTask : public WebGrep::noncopyable
{
  //constructor for a root node
  LinkedTask();
public:

  static std::shared_ptr<LinkedTask> createRootNode();

  //constructor for child nodes
  LinkedTask(LinkedTask* rootNode);

  //shallow copy without {.next, .targetUrl, .pageContent}
  void shallowCopy(const LinkedTask& other);

  /** Create subtree (level + 1) at (LinkedTask)child.load() pointer.
   * It'll replace old subtree if present, the caller must take ownership..
   *
   * @param expelledChild: ref. of a pointer to keep previous child,
   * call takes ownership and must delete all it's items as well.
   * @return new child node or NULL on caught exception.
 */
  LinkedTask* spawnChildNode(LinkedTask*& expelledChild);

  /** traverse to the last item on current tree level
   * @return last item if exists, (this) otherwise. */
  LinkedTask* getLastOnLevel();

  /** Append items to (.next) node (on the same tree level)*/
  size_t spawnNextNodes(size_t nodesCount);

  /** Scan targetVariables.matchURLVector[] and create linked list of subtasks on CURRENT LEVEL.
   * @return quantity of subtasks spawned. */
  size_t spawnGreppedSubtasks(const std::string& host_and_port, const GrepVars& targetVariables, size_t skipCount = 0);

  //level of this node
  unsigned level, order;
  //counds next/child nodes: load() is acquire
  std::atomic_uint childNodesCount;

  /** (LinkedTask*)next points to same level item,
   *  (LinkedTask*)child points to next level items(subtree).
   * load : memory_order_acquire
   * store: memory_order_release */
  std::atomic_uintptr_t next, child, root, parent;

  GrepVars grepVars;

  // you must have guaranteed that these are set & will live longer than any LinkedTask object
  std::shared_ptr<std::atomic_uint> linksCounterPtr, maxLinksCountPtr;

  std::atomic_uint nodeAllocationsCount;

  /** ctor() sets the limit 8192 that sis computed for estimation of 2GB memory for 200kb .html pages in average.
   * You can modify this value for root node if it's reasonable.
 */
  std::atomic_uint maxPossbleNodesQuantity;

  //modify these for root node, all child nodes will use the (LinkedTask*)root pointer to call them.
  //child nodes usuall have these equal to (nullptr)
  std::function<LinkedTask*(LinkedTask*/*RootNodePtr*/)> makeNewNode;
  std::function<void(LinkedTask*/*RootNodePtr*/, LinkedTask* /*node_ptr*/)> deleteNode;

};
//---------------------------------------------------------------
typedef std::shared_ptr<LinkedTask> RootNodePtr;

//---------------------------------------------------------------
static inline LinkedTask* ItemLoadAcquire(std::atomic_uintptr_t& value)
{ return (LinkedTask*)value.load(std::memory_order_acquire); }

static inline LinkedTask* ItemLoadRelaxed(std::atomic_uintptr_t& value)
{ return (LinkedTask*)value.load(std::memory_order_relaxed); }


static inline std::atomic_uintptr_t& StoreAcquire(std::atomic_uintptr_t& atom, LinkedTask* ptr)
{
  atom.store((std::uintptr_t)ptr, std::memory_order_acquire);
  return atom;
}

static inline std::atomic_uintptr_t& StoreRelease(std::atomic_uintptr_t& atom, LinkedTask* ptr)
{
  atom.store((std::uintptr_t)ptr, std::memory_order_release);
  return atom;
}
/** Extracts "site.com:443" from https://site.com:443/some/path */
std::string ExtractHostPortHttp(const std::string& targetUrl);

/** Returns index of first char after "://" sequence
 *  or value equal to nmax if not found.
 * @return nmax if not found.
*/
size_t FindURLAddressBegin(const char* str, size_t nmax = std::string::npos);

/** Returns index of first '/' after "://" sequence or string begin if it's absent.
 * @return nmax if not found.
*/
size_t FindURLPathBegin(const char* str, size_t nmax);

//returns position of closing quaote '\"' or other symbols like "'" or ">",
//if returned value >= (end - strPtr) then the quote is missing
size_t FindClosingQuote(const char* strPtr, const char* end);

/** Unfold the short link into a full path using info from (targetVars)
 * @return full URL like http://site.com/some/path/file.html
*/
std::string MakeFullPath(const char* url, size_t len,
                         const std::string& host_and_port,
                         const WebGrep::GrepVars& targetVars);

}//WebGrep

#endif // LINKEDTASK_H
