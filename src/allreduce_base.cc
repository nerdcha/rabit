/*!
 *  Copyright (c) 2014 by Contributors
 * \file allreduce_base.cc
 * \brief Basic implementation of AllReduce
 *
 * \author Tianqi Chen, Ignacio Cano, Tianyi Zhou
 */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE
#define NOMINMAX
#include <map>
#include <cstdlib>
#include <cstring>
#include "./allreduce_base.h"
#include "../include/rabit/rabit-inl.h"

namespace rabit {
namespace engine {
// constructor
AllreduceBase::AllreduceBase(void) {
  tracker_uri = "NULL";
  tracker_port = 9000;
  host_uri = "";
  slave_port = 9010;
  nport_trial = 1000;
  rank = 0;
  world_size = -1;
  hadoop_mode = 0;
  version_number = 0;
  task_id = "NULL";
  err_link = NULL;
  this->SetParam("rabit_reduce_buffer", "256MB");
  // running step for loop
  approx_run_step = 0.001;
  approx_check_step = 0.3;
  approx_check_min_step = 0.01;
}

// initialization function
void AllreduceBase::Init(void) {
  // setup from enviroment variables
  {
    // handling for hadoop
    const char *task_id = getenv("mapred_tip_id");
    if (task_id == NULL) {
      task_id = getenv("mapreduce_task_id");
    }
    if (hadoop_mode != 0) {
      utils::Check(task_id != NULL,
                   "hadoop_mode is set but cannot find mapred_task_id");
    }
    if (task_id != NULL) {
      this->SetParam("rabit_task_id", task_id);
      this->SetParam("rabit_hadoop_mode", "1");
    }
    const char *attempt_id = getenv("mapred_task_id");
    if (attempt_id != 0) {
      const char *att = strrchr(attempt_id, '_');
      int num_trial;
      if (att != NULL && sscanf(att + 1, "%d", &num_trial) == 1) {
        this->SetParam("rabit_num_trial", att + 1);
      }
    }
    // handling for hadoop
    const char *num_task = getenv("mapred_map_tasks");
    if (num_task == NULL) {
      num_task = getenv("mapreduce_job_maps");
    }
    if (hadoop_mode != 0) {
      utils::Check(num_task != NULL,
                   "hadoop_mode is set but cannot find mapred_map_tasks");
    }
    if (num_task != NULL) {
      this->SetParam("rabit_world_size", num_task);
    }
  }
  // clear the setting before start reconnection
  this->rank = -1;
  //---------------------
  // start socket
  utils::Socket::Startup();
  utils::Assert(all_links.size() == 0, "can only call Init once");
  this->host_uri = utils::SockAddr::GetHostName();
  // get information from tracker
  this->ReConnectLinks();
}

void AllreduceBase::Shutdown(void) {
  for (size_t i = 0; i < all_links.size(); ++i) {
    all_links[i].sock.Close();
  }
  all_links.clear();
  tree_links.plinks.clear();

  if (tracker_uri == "NULL") return;
  // notify tracker rank i have shutdown
  utils::TCPSocket tracker = this->ConnectTracker();
  tracker.SendStr(std::string("shutdown"));
  tracker.Close();
  utils::TCPSocket::Finalize();
}
void AllreduceBase::TrackerPrint(const std::string &msg) {
  if (tracker_uri == "NULL") {
    utils::Printf("%s", msg.c_str()); return;
  }
  utils::TCPSocket tracker = this->ConnectTracker();
  tracker.SendStr(std::string("print"));
  tracker.SendStr(msg);
  tracker.Close();
}
/*!
 * \brief set parameters to the engine 
 * \param name parameter name
 * \param val parameter value
 */
void AllreduceBase::SetParam(const char *name, const char *val) {
  if (!strcmp(name, "rabit_tracker_uri")) tracker_uri = val;
  if (!strcmp(name, "rabit_tracker_port")) tracker_port = atoi(val);
  if (!strcmp(name, "rabit_task_id")) task_id = val;
  if (!strcmp(name, "rabit_world_size")) world_size = atoi(val);
  if (!strcmp(name, "rabit_hadoop_mode")) hadoop_mode = atoi(val);
  if (!strcmp(name, "rabit_reduce_buffer")) {
    char unit;
    uint64_t amount;
    if (sscanf(val, "%lu%c", &amount, &unit) == 2) {
      switch (unit) {
        case 'B': reduce_buffer_size = (amount + 7)/ 8; break;
        case 'K': reduce_buffer_size = amount << 7UL; break;
        case 'M': reduce_buffer_size = amount << 17UL; break;
        case 'G': reduce_buffer_size = amount << 27UL; break;
        default: utils::Error("invalid format for reduce buffer");
      }
    } else {
      utils::Error("invalid format for reduce_buffer,"\
                   "shhould be {integer}{unit}, unit can be {B, KB, MB, GB}");
    }
  }
}
/*!
 * \brief initialize connection to the tracker
 * \return a socket that initializes the connection
 */
utils::TCPSocket AllreduceBase::ConnectTracker(void) const {
  int magic = kMagic;
  // get information from tracker
  utils::TCPSocket tracker;
  tracker.Create();
  if (!tracker.Connect(utils::SockAddr(tracker_uri.c_str(), tracker_port))) {
    utils::Socket::Error("Connect");
  }
  using utils::Assert;
  Assert(tracker.SendAll(&magic, sizeof(magic)) == sizeof(magic),
         "ReConnectLink failure 1");
  Assert(tracker.RecvAll(&magic, sizeof(magic)) == sizeof(magic),
         "ReConnectLink failure 2");
  utils::Check(magic == kMagic, "sync::Invalid tracker message, init failure");
  Assert(tracker.SendAll(&rank, sizeof(rank)) == sizeof(rank),
                "ReConnectLink failure 3");
  Assert(tracker.SendAll(&world_size, sizeof(world_size)) == sizeof(world_size),
         "ReConnectLink failure 3");
  tracker.SendStr(task_id);
  return tracker;
}
/*!
 * \brief connect to the tracker to fix the the missing links
 *   this function is also used when the engine start up
 */
void AllreduceBase::ReConnectLinks(const char *cmd) {
  // single node mode
  if (tracker_uri == "NULL") {
    rank = 0; world_size = 1; return;
  }
  utils::TCPSocket tracker = this->ConnectTracker();
  tracker.SendStr(std::string(cmd));

  // the rank of previous link, next link in ring
  int prev_rank, next_rank;
  // the rank of neighbors
  std::map<int, int> tree_neighbors;
  using utils::Assert;
  // get new ranks
  int newrank, num_neighbors;
  Assert(tracker.RecvAll(&newrank, sizeof(newrank)) == sizeof(newrank),
           "ReConnectLink failure 4");
  Assert(tracker.RecvAll(&parent_rank, sizeof(parent_rank)) ==\
         sizeof(parent_rank), "ReConnectLink failure 4");
  Assert(tracker.RecvAll(&world_size, sizeof(world_size)) == sizeof(world_size),
         "ReConnectLink failure 4");
  Assert(rank == -1 || newrank == rank,
         "must keep rank to same if the node already have one");
  rank = newrank;
  Assert(tracker.RecvAll(&num_neighbors, sizeof(num_neighbors)) ==  \
         sizeof(num_neighbors), "ReConnectLink failure 4");
  for (int i = 0; i < num_neighbors; ++i) {
    int nrank;
    Assert(tracker.RecvAll(&nrank, sizeof(nrank)) == sizeof(nrank),
           "ReConnectLink failure 4");
    tree_neighbors[nrank] = 1;
  }
  Assert(tracker.RecvAll(&prev_rank, sizeof(prev_rank)) == sizeof(prev_rank),
         "ReConnectLink failure 4");
  Assert(tracker.RecvAll(&next_rank, sizeof(next_rank)) == sizeof(next_rank),
         "ReConnectLink failure 4");
  // create listening socket
  utils::TCPSocket sock_listen;
  sock_listen.Create();
  int port = sock_listen.TryBindHost(slave_port, slave_port + nport_trial);
  utils::Check(port != -1, "ReConnectLink fail to bind the ports specified");
  sock_listen.Listen();

  // get number of to connect and number of to accept nodes from tracker
  int num_conn, num_accept, num_error = 1;
  do {
    // send over good links
    std::vector<int> good_link;
    for (size_t i = 0; i < all_links.size(); ++i) {
      if (!all_links[i].sock.BadSocket()) {
        good_link.push_back(static_cast<int>(all_links[i].rank));
      } else {
        if (!all_links[i].sock.IsClosed()) all_links[i].sock.Close();
      }
    }    
    int ngood = static_cast<int>(good_link.size());
    Assert(tracker.SendAll(&ngood, sizeof(ngood)) == sizeof(ngood),
           "ReConnectLink failure 5");
    for (size_t i = 0; i < good_link.size(); ++i) {
      Assert(tracker.SendAll(&good_link[i], sizeof(good_link[i])) == \
             sizeof(good_link[i]), "ReConnectLink failure 6");
    }
    Assert(tracker.RecvAll(&num_conn, sizeof(num_conn)) == sizeof(num_conn),
           "ReConnectLink failure 7");
    Assert(tracker.RecvAll(&num_accept, sizeof(num_accept)) ==  \
           sizeof(num_accept), "ReConnectLink failure 8");
    num_error = 0;
    for (int i = 0; i < num_conn; ++i) {
      LinkRecord r;
      int hport, hrank;
      std::string hname;
      tracker.RecvStr(&hname);
      Assert(tracker.RecvAll(&hport, sizeof(hport)) == sizeof(hport),
             "ReConnectLink failure 9");
      Assert(tracker.RecvAll(&hrank, sizeof(hrank)) == sizeof(hrank),
             "ReConnectLink failure 10");
      r.sock.Create();
      if (!r.sock.Connect(utils::SockAddr(hname.c_str(), hport))) {
        num_error += 1; r.sock.Close(); continue;
      }
      Assert(r.sock.SendAll(&rank, sizeof(rank)) == sizeof(rank),
             "ReConnectLink failure 12");
      Assert(r.sock.RecvAll(&r.rank, sizeof(r.rank)) == sizeof(r.rank),
             "ReConnectLink failure 13");
      utils::Check(hrank == r.rank,
                   "ReConnectLink failure, link rank inconsistent");
      bool match = false;
      for (size_t i = 0; i < all_links.size(); ++i) {
        if (all_links[i].rank == hrank) {
          Assert(all_links[i].sock.IsClosed(),
                 "Override a link that is active");
          all_links[i].sock = r.sock; match = true; break;
        }
      }
      if (!match) all_links.push_back(r);
    }
    Assert(tracker.SendAll(&num_error, sizeof(num_error)) == sizeof(num_error),
           "ReConnectLink failure 14");
  } while (num_error != 0);
  // send back socket listening port to tracker
  Assert(tracker.SendAll(&port, sizeof(port)) == sizeof(port),
         "ReConnectLink failure 14");
  // close connection to tracker
  tracker.Close();
  // listen to incoming links
  for (int i = 0; i < num_accept; ++i) {
    LinkRecord r;
    r.sock = sock_listen.Accept();
    Assert(r.sock.SendAll(&rank, sizeof(rank)) == sizeof(rank),
           "ReConnectLink failure 15");
    Assert(r.sock.RecvAll(&r.rank, sizeof(r.rank)) == sizeof(r.rank),
           "ReConnectLink failure 15");
    bool match = false;
    for (size_t i = 0; i < all_links.size(); ++i) {
      if (all_links[i].rank == r.rank) {
        utils::Assert(all_links[i].sock.IsClosed(),
                      "Override a link that is active");
        all_links[i].sock = r.sock; match = true; break;
      }
    }
    if (!match) all_links.push_back(r);
  }
  // close listening sockets
  sock_listen.Close();
  this->parent_index = -1;
  // setup tree links and ring structure
  tree_links.plinks.clear();
  for (size_t i = 0; i < all_links.size(); ++i) {
    utils::Assert(!all_links[i].sock.BadSocket(), "ReConnectLink: bad socket");
    // set the socket to non-blocking mode, enable TCP keepalive
    all_links[i].sock.SetNonBlock(true);
    all_links[i].sock.SetKeepAlive(true);
    if (tree_neighbors.count(all_links[i].rank) != 0) {
      if (all_links[i].rank == parent_rank) {
        parent_index = static_cast<int>(tree_links.plinks.size());
      }
      tree_links.plinks.push_back(&all_links[i]);
    }
    if (all_links[i].rank == prev_rank) ring_prev = &all_links[i];
    if (all_links[i].rank == next_rank) ring_next = &all_links[i];
  }
  Assert(parent_rank == -1 || parent_index != -1,
         "cannot find parent in the link");
  Assert(prev_rank == -1 || ring_prev != NULL,
         "cannot find prev ring in the link");
  Assert(next_rank == -1 || ring_next != NULL,
         "cannot find next ring in the link");
}
/*!
 * \brief perform in-place allreduce, on sendrecvbuf, this function can fail, and will return the cause of failure
 *
 * NOTE on Allreduce:
 *    The kSuccess TryAllreduce does NOT mean every node have successfully finishes TryAllreduce.
 *    It only means the current node get the correct result of Allreduce.
 *    However, it means every node finishes LAST call(instead of this one) of Allreduce/Bcast
 * 
 * \param sendrecvbuf_ buffer for both sending and recving data
 * \param type_nbytes the unit number of bytes the type have
 * \param count number of elements to be reduced
 * \param reducer reduce function
 * \param exec pointer to executor class
 * \return this function can return kSuccess, kSockError, kGetExcept, see ReturnType for details
 * \sa ReturnType
 */
AllreduceBase::ReturnType
AllreduceBase::TryAllreduce(void *sendrecvbuf_,
                            size_t type_nbytes,
                            size_t count,
                            ReduceFunction reducer,
                            PreprocLoopExecutor *exec) {
  RefLinkVector &links = tree_links;
  if (links.size() == 0 || count == 0) return kSuccess;
  // total size of message
  const size_t total_size = type_nbytes * count;
  // number of links
  const int nlink = static_cast<int>(links.size());
  // send recv buffer
  char *sendrecvbuf = reinterpret_cast<char*>(sendrecvbuf_);
  // size of space that we already performs reduce in up pass
  size_t size_up_reduce = 0;
  // size of space that we have already passed to parent
  size_t size_up_out = 0;
  // size of message we received, and send in the down pass
  size_t size_down_in = 0;
  // initialize the link ring-buffer and pointer
  for (int i = 0; i < nlink; ++i) {
    if (i != parent_index) {
      links[i].InitBuffer(type_nbytes, count, reduce_buffer_size);
    }
    links[i].ResetSize();
  }
  // if no childs, no need to reduce
  if (nlink == static_cast<int>(parent_index != -1)) {
    size_up_reduce = total_size;
  }
  // while we have not passed the messages out
  while (true) {
    // select helper
    bool finished = true;
    utils::SelectHelper selecter;
    for (int i = 0; i < nlink; ++i) {
      if (i == parent_index) {
        if (size_down_in != total_size) {
          selecter.WatchRead(links[i].sock);
          // only watch for exception in live channels
          selecter.WatchException(links[i].sock);
          finished = false;
        }
        if (size_up_out != total_size && size_up_out < size_up_reduce) {
          selecter.WatchWrite(links[i].sock);
        }
      } else {
        if (links[i].size_read != total_size) {
          selecter.WatchRead(links[i].sock);
        }
        // size_write <= size_read
        if (links[i].size_write != total_size){
          if (links[i].size_write < size_down_in) {
            selecter.WatchWrite(links[i].sock);
          }
          // only watch for exception in live channels
          selecter.WatchException(links[i].sock);
          finished = false;
        }
      }
    }
    // finish runing allreduce
    if (finished) break;
    if (exec != NULL) {      
      exec->Run();
      if (exec->LoopEnd()) {
        selecter.Select();
      } else {
        // use non-blocking selection
        selecter.Select(0);
      }
    } else {
      selecter.Select();  
    }
    // exception handling
    for (int i = 0; i < nlink; ++i) {
      // recive OOB message from some link
      if (selecter.CheckExcept(links[i].sock)) {
        return ReportError(&links[i], kGetExcept);
      }
    }
    // read data from childs
    for (int i = 0; i < nlink; ++i) {
      if (i != parent_index && selecter.CheckRead(links[i].sock)) {
        ReturnType ret = links[i].ReadToRingBuffer(size_up_out);
        if (ret != kSuccess) {
          return ReportError(&links[i], ret);
        }
      }
    }
    // this node have childs, peform reduce
    if (nlink > static_cast<int>(parent_index != -1)) {
      size_t buffer_size = 0;
      // do upstream reduce
      size_t max_reduce = total_size;
      for (int i = 0; i < nlink; ++i) {
        if (i != parent_index) {
          max_reduce= std::min(max_reduce, links[i].size_read);
          utils::Assert(buffer_size == 0 || buffer_size == links[i].buffer_size,
                        "buffer size inconsistent");
          buffer_size = links[i].buffer_size;
        }
      }
      utils::Assert(buffer_size != 0, "must assign buffer_size");
      // round to type_n4bytes
      max_reduce = (max_reduce / type_nbytes * type_nbytes);
      // peform reduce, can be at most two rounds
      while (size_up_reduce < max_reduce) {
        // start position
        size_t start = size_up_reduce % buffer_size;
        // peform read till end of buffer
        size_t nread = std::min(buffer_size - start,
                                max_reduce - size_up_reduce);
        utils::Assert(nread % type_nbytes == 0, "Allreduce: size check");
        for (int i = 0; i < nlink; ++i) {
          if (i != parent_index) {
            reducer(links[i].buffer_head + start,
                    sendrecvbuf + size_up_reduce,
                    static_cast<int>(nread / type_nbytes),
                    MPI::Datatype(type_nbytes));
          }
        }
        size_up_reduce += nread;
      }
    }
    if (parent_index != -1) {
      // pass message up to parent, can pass data that are already been reduced
      if (size_up_out < size_up_reduce) {
        ssize_t len = links[parent_index].sock.
            Send(sendrecvbuf + size_up_out, size_up_reduce - size_up_out);
        if (len != -1) {
          size_up_out += static_cast<size_t>(len);
        } else {
          ReturnType ret = Errno2Return(errno);
          if (ret != kSuccess) {
            return ReportError(&links[parent_index], ret);
          }
        }
      }
      // read data from parent
      if (selecter.CheckRead(links[parent_index].sock) &&
          total_size > size_down_in) {
        ssize_t len = links[parent_index].sock.
            Recv(sendrecvbuf + size_down_in, total_size - size_down_in);
        if (len == 0) {
          links[parent_index].sock.Close(); 
          return ReportError(&links[parent_index], kRecvZeroLen);
        }
        if (len != -1) {
          size_down_in += static_cast<size_t>(len);
          utils::Assert(size_down_in <= size_up_out,
                        "Allreduce: boundary error");
        } else {
          ReturnType ret = Errno2Return(errno);
          if (ret != kSuccess) {
            return ReportError(&links[parent_index], ret);
          }
        }
      }
    } else {
      // this is root, can use reduce as most recent point
      size_down_in = size_up_out = size_up_reduce;
    }
    // can pass message down to childs
    for (int i = 0; i < nlink; ++i) {
      if (i != parent_index && links[i].size_write < size_down_in) {
        ReturnType ret = links[i].WriteFromArray(sendrecvbuf, size_down_in);
        if (ret != kSuccess) {
          return ReportError(&links[i], ret);
        }
      }
    }
  }
  return kSuccess;
}
/*!
 * \brief broadcast data from root to all nodes, this function can fail,and will return the cause of failure
 * \param sendrecvbuf_ buffer for both sending and recving data
 * \param total_size the size of the data to be broadcasted
 * \param root the root worker id to broadcast the data
 * \return this function can return kSuccess, kSockError, kGetExcept, see ReturnType for details
 * \sa ReturnType
 */
AllreduceBase::ReturnType
AllreduceBase::TryBroadcast(void *sendrecvbuf_, size_t total_size, int root) {
  RefLinkVector &links = tree_links;
  if (links.size() == 0 || total_size == 0) return kSuccess;
  utils::Check(root < world_size,
               "Broadcast: root should be smaller than world size");
  // number of links
  const int nlink = static_cast<int>(links.size());
  // size of space already read from data
  size_t size_in = 0;
  // input link, -2 means unknown yet, -1 means this is root
  int in_link = -2;

  // initialize the link statistics
  for (int i = 0; i < nlink; ++i) {
    links[i].ResetSize();
  }
  // root have all the data
  if (this->rank == root) {
    size_in = total_size;
    in_link = -1;
  }
  // while we have not passed the messages out
  while (true) {
    bool finished = true;
    // select helper
    utils::SelectHelper selecter;
    for (int i = 0; i < nlink; ++i) {
      if (in_link == -2) {
        selecter.WatchRead(links[i].sock); finished = false;
      }
      if (i == in_link && links[i].size_read != total_size) {
        selecter.WatchRead(links[i].sock); finished = false;
      }
      if (in_link != -2 && i != in_link && links[i].size_write != total_size) {
        if (links[i].size_write < size_in) {
          selecter.WatchWrite(links[i].sock);
        }
        finished = false;
      }
      selecter.WatchException(links[i].sock);
    }
    // finish running
    if (finished) break;
    // select
    selecter.Select();
    // exception handling
    for (int i = 0; i < nlink; ++i) {
      // recive OOB message from some link
      if (selecter.CheckExcept(links[i].sock)) {
        return ReportError(&links[i], kGetExcept);
      }
    }
    if (in_link == -2) {
      // probe in-link
      for (int i = 0; i < nlink; ++i) {
        if (selecter.CheckRead(links[i].sock)) {
          ReturnType ret = links[i].ReadToArray(sendrecvbuf_, total_size);
          if (ret != kSuccess) {
            return ReportError(&links[i], ret);
          }
          size_in = links[i].size_read;
          if (size_in != 0) {
            in_link = i; break;
          }
        }
      }
    } else {
      // read from in link
      if (in_link >= 0 && selecter.CheckRead(links[in_link].sock)) {
        ReturnType ret = links[in_link].ReadToArray(sendrecvbuf_, total_size);
        if (ret != kSuccess) {
          return ReportError(&links[in_link], ret);
        }
        size_in = links[in_link].size_read;
      }
    }
    // send data to all out-link
    for (int i = 0; i < nlink; ++i) {
      if (i != in_link && links[i].size_write < size_in) {
        ReturnType ret = links[i].WriteFromArray(sendrecvbuf_, size_in);
        if (ret != kSuccess) {
          return ReportError(&links[i], ret);
        }
      }
    }
  }
  return kSuccess;
}
// struct to record loop status
struct LoopStatus {
  // number of nodes that left
  size_t num_left;
  // maximum number of left loop
  size_t max_left;
  // number of nodes that finishs the job
  size_t num_finish;
  explicit LoopStatus(size_t num_left)
      : num_left(num_left), max_left(num_left),
        num_finish(num_left == 0) {
  }
  // reducer for Allreduce, get the result ActionSummary from all nodes
  inline static void Reducer(const void *src_, void *dst_,
                             int len, const MPI::Datatype &dtype) {
    const LoopStatus *src = reinterpret_cast<const LoopStatus*>(src_);
    LoopStatus *dst = reinterpret_cast<LoopStatus*>(dst_);
    dst->num_left += src->num_left;
    dst->max_left = std::max(dst->max_left, src->max_left);
    dst->num_finish += src->num_finish;
  }
};
/*!
 * \brief execute the prepare_loop until approximate level is reached
 * \param prepare_loop Lazy preprocessing loop, prepare_loop(prepare_arg, begin, end)
 *                     will be called by the function before performing Allreduce
 *                     in order to initialize the data in sendrecvbuf.
 *                     If the result of Allreduce can be recovered directly, then prepare_loop will NOT be called
 * \param prepare_arg argument used to pass into the lazy preprocessing function
 * \param num_loop_iter the number of loop iteration to be called for a complete preprocessing
 * \param approx_ratio approximate ratio we can tolerant
 */
AllreduceBase::ReturnType
AllreduceBase::TryExecLoop(PreprocLoopFunction prepare_loop,
                           void *prepare_arg,
                           size_t num_loop_iter,
                           double approx_ratio,
                           double *out_rapprox) {
  // total number of resources to complete
  size_t num_total = num_loop_iter;
  ReturnType ret = TryAllreduce(&num_total, sizeof(num_total), 1,
                                op::Reducer<op::Sum, size_t>);
  if (ret != kSuccess) return ret;
  PreprocLoopExecutor exec;
  exec.prepare_loop = prepare_loop;
  exec.prepare_arg = prepare_arg;
  exec.num_loop_iter = num_loop_iter;
  exec.loop_step = static_cast<size_t>(num_total * approx_run_step / world_size);
  if (exec.loop_step == 0) exec.loop_step = 1;
  // number of data pts left
  size_t num_left = num_total;
  size_t approx_gap = num_total - static_cast<size_t>(approx_ratio * num_total);
  if (approx_gap == 0) {
    exec.Run(num_loop_iter);
    if (out_rapprox != NULL) {
      *out_rapprox = 1.0;
    }
    return kSuccess;
  }
  // loop check
  while (num_left != 0) {
    size_t step = static_cast<size_t>(num_left * approx_check_step / world_size);
    step = std::max(step, static_cast<size_t>(num_total * approx_check_min_step / world_size));
    if (step < exec.loop_step) step = exec.loop_step;
    exec.Run(step);
    LoopStatus status(num_loop_iter - exec.loop_counter);
    ReturnType ret = TryAllreduce(&status, sizeof(status), 1,
                                  LoopStatus::Reducer, &exec);
    if (ret != kSuccess) return ret;
    num_left = status.num_left;
    if (num_left < approx_gap &&
        status.num_finish > world_size * 0.5) {
      break;
    }
  }
  if (num_left != 0) {
    LoopStatus status(num_loop_iter - exec.loop_counter);
    ReturnType ret = TryAllreduce(&status, sizeof(status), 1,
                                  LoopStatus::Reducer);
    if (ret != kSuccess) return ret;
    num_left = status.num_left;
  }
  if (out_rapprox != NULL) {
    *out_rapprox = static_cast<double>(num_total - num_left) / num_total;
  }
  return kSuccess;
}
}  // namespace engine
}  // namespace rabit
