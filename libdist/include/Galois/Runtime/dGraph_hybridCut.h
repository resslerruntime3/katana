/** partitioned graph wrapper for vertexCut -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
 * @section Contains the vertex cut functionality to be used in dGraph.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 */

#include <vector>
#include <set>
#include <algorithm>
#include <unordered_map>
#include "Galois/Runtime/dGraph.h"
#include <boost/dynamic_bitset.hpp>
#include "Galois/Runtime/vecBool_bitset.h"
#include "Galois/Runtime/dGraph_edgeAssign_policy.h"
#include "Galois/Runtime/Dynamic_bitset.h"
#include "Galois/Graphs/FileGraph.h"
#include <sstream>

#include "Galois/DoAllWrap.h"

#define BATCH_MSG_SIZE 1000
template<typename NodeTy, typename EdgeTy, bool BSPNode = false, bool BSPEdge = false>
class hGraph_vertexCut : public hGraph<NodeTy, EdgeTy, BSPNode, BSPEdge> {

  public:
    typedef hGraph<NodeTy, EdgeTy, BSPNode, BSPEdge> base_hGraph;
    /** Utilities for reading partitioned graphs. **/
    struct NodeInfo {
      NodeInfo() :
        local_id(0), global_id(0), owner_id(0) {
        }
      NodeInfo(size_t l, size_t g, size_t o) :
        local_id(l), global_id(g), owner_id(o) {
        }
      size_t local_id;
      size_t global_id;
      size_t owner_id;
    };


    std::vector<NodeInfo> localToGlobalMap_meta;
    std::vector<size_t> OwnerVec; //To store the ownerIDs of sorted according to the Global IDs.
    std::vector<uint64_t> GlobalVec; //Global Id's sorted vector.
    std::vector<std::pair<uint32_t, uint32_t>> hostNodes;

    std::vector<size_t> GlobalVec_ordered; //Global Id's sorted vector.
    // To send edges to different hosts: #Src #Dst
    std::vector<std::vector<uint64_t>> assigned_edges_perhost;
    std::vector<uint64_t> recv_assigned_edges;
    uint64_t num_total_edges_to_receive;

    // GID = localToGlobalVector[LID]
    std::vector<uint64_t> localToGlobalVector;
    // LID = globalToLocalMap[GID]
    std::unordered_map<uint64_t, uint32_t> globalToLocalMap;



    //EXPERIMENT
    std::unordered_map<uint64_t, uint32_t> GlobalVec_map;

    //XXX: initialize to ~0
    std::vector<uint64_t> numNodes_per_host;

    //XXX: Use EdgeTy to determine if need to load edge weights or not.
    using Host_edges_map_type = typename std::conditional<!std::is_void<EdgeTy>::value, std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, uint32_t>>> , std::unordered_map<uint64_t, std::vector<uint64_t>>>::type;
    Host_edges_map_type host_edges_map;
    //std::unordered_map<uint64_t, std::vector<uint64_t>> host_edges_map;
    std::vector<uint64_t> numEdges_per_host;
    std::vector<std::pair<uint64_t, uint64_t>> gid2host;
    std::vector<std::pair<uint64_t, uint64_t>> gid2host_withoutEdges;

    Galois::VecBool gid_vecBool;

    uint64_t globalOffset;
    uint32_t numNodes;
    bool isBipartite;
    uint64_t last_nodeID_withEdges_bipartite;
    uint64_t numEdges;

    unsigned getHostID(uint64_t gid) const {
      auto lid = G2L(gid);
      return OwnerVec[lid];
    }

    size_t getOwner_lid(size_t lid) const {
      return OwnerVec[lid];
    }

#if 0
    virtual bool isLocal(uint64_t gid) const {
      return (GlobalVec_map.find(gid) != GlobalVec_map.end());
    }

#endif


    bool isOwned(uint64_t gid) const {
        if(gid >= gid2host[base_hGraph::id].first && gid < gid2host[base_hGraph::id].second)
          return true;
        else
          return false;
    }
#if 0
    bool isOwned(uint64_t gid) const {
      for(auto i : hostNodes){
        if (i.first != (uint64_t)(~0)) {
          //assert(i.first < GlobalVec.size());
          //assert(i.second <= GlobalVec.size());
          auto iter = std::lower_bound(GlobalVec.begin() + i.first, GlobalVec.begin() + i.second, gid);
          if((iter != (GlobalVec.begin() + i.second)) && (*iter == gid)){
            return true;
          }
        }
      }
      return false;
    }
#endif

  virtual bool isLocal(uint64_t gid) const {
    assert(gid < base_hGraph::totalNodes);
    if (isOwned(gid))
      return true;
    return (globalToLocalMap.find(gid) != globalToLocalMap.end());
  }

  virtual uint32_t G2L(uint64_t gid) const {
    assert(isLocal(gid));
    return globalToLocalMap.at(gid);
  }

  virtual uint64_t L2G(uint32_t lid) const {
    return localToGlobalVector[lid];
  }


    std::string getMetaFileName(const std::string & basename, unsigned hostID, unsigned num_hosts){
      std::string result = basename;
      result+= ".META.";
      result+=std::to_string(hostID);
      result+= ".OF.";
      result+=std::to_string(num_hosts);
      return result;
    }

    bool readMetaFile(const std::string& metaFileName, std::vector<NodeInfo>& localToGlobalMap_meta){
      std::ifstream meta_file(metaFileName, std::ifstream::binary);
      if (!meta_file.is_open()) {
        std::cerr << "Unable to open file " << metaFileName << "! Exiting!\n";
        return false;
      }
      size_t num_entries;
      meta_file.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));
      std::cout << "Partition :: " << " Number of nodes :: " << num_entries << "\n";
      for (size_t i = 0; i < num_entries; ++i) {
        std::pair<size_t, size_t> entry;
        size_t owner;
        meta_file.read(reinterpret_cast<char*>(&entry.first), sizeof(entry.first));
        meta_file.read(reinterpret_cast<char*>(&entry.second), sizeof(entry.second));
        meta_file.read(reinterpret_cast<char*>(&owner), sizeof(owner));
        localToGlobalMap_meta.push_back(NodeInfo(entry.second, entry.first, owner));
      }
      return true;
    }

    std::string getPartitionFileName(const std::string & basename, unsigned hostID, unsigned num_hosts){
      std::string result = basename;
      result+= ".PART.";
      result+=std::to_string(hostID);
      result+= ".OF.";
      result+=std::to_string(num_hosts);
      return result;
    }

    std::pair<uint32_t, uint32_t> nodes_by_host(uint32_t host) const {
      return std::make_pair<uint32_t, uint32_t>(~0,~0);
    }

    std::pair<uint64_t, uint64_t> nodes_by_host_G(uint32_t host) const {
      return std::make_pair<uint64_t, uint64_t>(~0,~0);
    }


    /**
     * Constructor for Vertex Cut
     */
    hGraph_vertexCut(const std::string& filename, 
               const std::string& partitionFolder,
               unsigned host, unsigned _numHosts, 
               std::vector<unsigned> scalefactor, 
               bool transpose = false, 
               uint32_t VCutThreshold = 100, 
               bool bipartite = false) : base_hGraph(host, _numHosts) {
      if (!scalefactor.empty()) {
        if (base_hGraph::id == 0) {
          std::cerr << "WARNING: scalefactor not supported for PowerLyra (hybrid) vertex-cuts\n";
        }
        scalefactor.clear();
      }
      if (balanceEdges) {
        if (base_hGraph::id == 0) {
          std::cerr << "WARNING: balanceEdges not supported for cartesian "
                       " vertex-cuts\n";
        }
        balanceEdges = false;
      }

      Galois::Runtime::reportStat("(NULL)", "ONLINE VERTEX CUT PL", 0, 0);

      Galois::Statistic statGhostNodes("TotalGhostNodes");
      Galois::StatTimer StatTimer_graph_construct("TIME_GRAPH_CONSTRUCT");
      Galois::StatTimer StatTimer_graph_construct_comm("TIME_GRAPH_CONSTRUCT_COMM");
      Galois::StatTimer StatTimer_local_distributed_edges("TIMER_LOCAL_DISTRIBUTE_EDGES");
      Galois::StatTimer StatTimer_exchange_edges("TIMER_EXCHANGE_EDGES");
      Galois::StatTimer StatTimer_fill_local_mirrorNodes("TIMER_FILL_LOCAL_MIRRORNODES");
      Galois::StatTimer StatTimer_distributed_edges_test_set_bit("TIMER_DISTRIBUTE_EDGES_TEST_SET_BIT");
      Galois::StatTimer StatTimer_allocate_local_DS("TIMER_ALLOCATE_LOCAL_DS");
      Galois::StatTimer StatTimer_distributed_edges_get_edges("TIMER_DISTRIBUTE_EDGES_GET_EDGES");
      Galois::StatTimer StatTimer_distributed_edges_inner_loop("TIMER_DISTRIBUTE_EDGES_INNER_LOOP");
      Galois::StatTimer StatTimer_distributed_edges_next_src("TIMER_DISTRIBUTE_EDGES_NEXT_SRC");

      StatTimer_graph_construct.start();
      //std::stringstream ss_cout;

      Galois::Graph::OfflineGraph g(filename);
      isBipartite = bipartite;

      uint64_t numNodes_to_divide = 0;
      if (isBipartite) {
    	  for (uint64_t n = 0; n < g.size(); ++n){
    		  if(std::distance(g.edge_begin(n), g.edge_end(n))){
                  ++numNodes_to_divide;
                  last_nodeID_withEdges_bipartite = n;
    		  }
    	  }
      }
      else {
    	  numNodes_to_divide = g.size();
      }

      //std::cout << "Nodes to divide : " <<  numNodes_to_divide << "\n";
      base_hGraph::totalNodes = g.size();
      base_hGraph::totalEdges = g.sizeEdges();
      std::cerr << "[" << base_hGraph::id << "] Total nodes : " << base_hGraph::totalNodes << " , Total edges : " << base_hGraph::totalEdges << "\n";
      //compute owners for all nodes
      if (scalefactor.empty() || (base_hGraph::numHosts == 1)) {
        for (unsigned i = 0; i < base_hGraph::numHosts; ++i)
          gid2host.push_back(Galois::block_range(
                               0U, (unsigned)numNodes_to_divide, i, 
                               base_hGraph::numHosts));
      }
#if 0
      else {
        assert(scalefactor.size() == base_hGraph::numHosts);
        unsigned numBlocks = 0;
        for (unsigned i = 0; i < base_hGraph::numHosts; ++i)
          numBlocks += scalefactor[i];
        std::vector<std::pair<uint64_t, uint64_t>> blocks;
        for (unsigned i = 0; i < numBlocks; ++i)
          blocks.push_back(Galois::block_range(0U, (unsigned)numNodes_to_divide, i, numBlocks));
        std::vector<unsigned> prefixSums;
        prefixSums.push_back(0);
        for (unsigned i = 1; i < base_hGraph::numHosts; ++i)
          prefixSums.push_back(prefixSums[i - 1] + scalefactor[i - 1]);
        for (unsigned i = 0; i < base_hGraph::numHosts; ++i) {
          unsigned firstBlock = prefixSums[i];
          unsigned lastBlock = prefixSums[i] + scalefactor[i] - 1;
          gid2host.push_back(std::make_pair(blocks[firstBlock].first, blocks[lastBlock].second));
        }
      }
#endif

      if (isBipartite) {
    	  uint64_t numNodes_without_edges = (g.size() - numNodes_to_divide);
    	  for (unsigned i = 0; i < base_hGraph::numHosts; ++i) {
    	    auto p = Galois::block_range(0U, 
                     (unsigned)numNodes_without_edges, 
                     i, base_hGraph::numHosts);
    	    //std::cout << " last node : " << last_nodeID_withEdges_bipartite << ", " << p.first << " , " << p.second << "\n";
    	    gid2host_withoutEdges.push_back(std::make_pair(last_nodeID_withEdges_bipartite + p.first + 1, last_nodeID_withEdges_bipartite + p.second + 1));
          }
      }

      uint64_t numEdges_distribute = g.edge_begin(gid2host[base_hGraph::id].second) - g.edge_begin(gid2host[base_hGraph::id].first); // depends on Offline graph impl
      std::cerr << "[" << base_hGraph::id << "] Total edges to distribute : " << numEdges_distribute << "\n";


      /********************************************
       * Assign edges to the hosts using heuristics
       * and send/recv from other hosts.
       * ******************************************/
      //print_string(" : assign_send_receive_edges started");
      StatTimer_exchange_edges.start();

      std::vector<uint64_t> prefixSumOfEdges;
      assign_edges_phase1(g, numEdges_distribute, VCutThreshold, 
                          prefixSumOfEdges, base_hGraph::mirrorNodes);

#if 0
      if(base_hGraph::id == 0)
      for(auto i = 0; i < base_hGraph::numHosts; ++i){
        std::cerr << "MIRROR : " << base_hGraph::mirrorNodes[i].size() <<"\n";
      }
#endif
      
      base_hGraph::numOwned = numNodes;
      base_hGraph::numNodes = base_hGraph::numNodes = numNodes;
      base_hGraph::numNodesWithEdges = base_hGraph::numNodes;
      base_hGraph::beginMaster = G2L(gid2host[base_hGraph::id].first);
      base_hGraph::endMaster = G2L(gid2host[base_hGraph::id].second - 1) + 1;
      //ss_cout << base_hGraph::id << " : numNodes : " << numNodes << " , numEdges : " << numEdges << "\n";

      /******************************************
       * Allocate and construct the graph
       *****************************************/
      //print_string(" : Allocate local graph DS : start");

      StatTimer_allocate_local_DS.start();

      if (!edgeNuma) {
        base_hGraph::graph.allocateFrom(numNodes, numEdges);
      } else {
        printf("Edge based NUMA division on\n");
        base_hGraph::graph.allocateFrom(numNodes, numEdges, prefixSumOfEdges);
      }

      base_hGraph::graph.constructNodes();

      for (uint32_t n = 0; n < numNodes; ++n) {
        base_hGraph::graph.fixEndEdge(n, prefixSumOfEdges[n]);
      }

      StatTimer_allocate_local_DS.stop();

      //print_string(" : Allocate local graph DS : Done");

      //print_string(" : loadEdges : start");

      loadEdges(base_hGraph::graph, g, numEdges_distribute, VCutThreshold);
      StatTimer_exchange_edges.stop();

      //print_string(" : loadEdges : done");

      //ss_cout << base_hGraph::id << " : assign_send_receive_edges done\n";

      /*******************************************/

      Galois::Runtime::getHostBarrier().wait();

      if (transpose && (numNodes > 0)) {
        base_hGraph::graph.transpose(edgeNuma);
        base_hGraph::transposed = true;
      }

      // TODO revise how this works and make it consistent across cuts
      if (!edgeNuma) {
        Galois::StatTimer StatTimer_thread_ranges("TIME_THREAD_RANGES");

        StatTimer_thread_ranges.start();
        base_hGraph::determine_thread_ranges(numNodes, prefixSumOfEdges);
        StatTimer_thread_ranges.stop();
      }

      StatTimer_graph_construct.stop();

      /*****************************************
       * Communication PreProcessing:
       * Exchange mirrors and master nodes among
       * hosts
       ****************************************/
      //print_string(" : Setup communication start");

      StatTimer_graph_construct_comm.start();

      base_hGraph::setup_communication();

      StatTimer_graph_construct_comm.stop();
      //print_string(" : Setup communication done");

    }

    template<typename GraphTy>
    void loadEdges(GraphTy& graph, Galois::Graph::OfflineGraph& g, uint64_t numEdges_distribute, uint32_t VCutThreshold){
      if (base_hGraph::id == 0) {
        if (std::is_void<typename GraphTy::edge_data_type>::value) {
          fprintf(stderr, "Loading void edge-data while creating edges.\n");
        } else {
          fprintf(stderr, "Loading edge-data while creating edges.\n");
        }
      }

      Galois::Timer timer;
      timer.start();
      g.reset_seek_counters();

      assigned_edges_perhost.resize(base_hGraph::numHosts);

      /*******************************************************
       * Galois:On_each loop for using multiple threads.
       * Thread 0 : Runs the assign_send_edges functions: To
       *            assign edges to hosts and send across.
       * Thread 1 : Runs the receive_edges functions: To
       *            edges assigned to this host by other hosts.
       *
       ********************************************************/
      Galois::on_each([&](unsigned tid, unsigned nthreads){
          if(tid == 0)
                assign_load_send_edges(graph, g, numEdges_distribute, VCutThreshold);
          if((nthreads == 1) || (tid == 1))
                receive_edges(graph);
          });

      ++Galois::Runtime::evilPhase;

      timer.stop();
      fprintf(stderr, "[%u] Edge loading time : %f seconds to read %lu bytes in %lu seeks\n", base_hGraph::id, timer.get_usec()/1000000.0f, g.num_bytes_read(), g.num_seeks());
    }

    // Just calculating the number of edges to send to other hosts
    void assign_edges_phase1(Galois::Graph::OfflineGraph& g, uint64_t numEdges_distribute, uint32_t VCutThreshold, std::vector<uint64_t>& prefixSumOfEdges, std::vector<std::vector<size_t>>& mirrorNodes){
      //Go over assigned nodes and distribute edges.

      auto& net = Galois::Runtime::getSystemNetworkInterface();
      std::vector<bool> ghosts(g.size());
      std::vector<std::vector<uint64_t>> numOutgoingEdges(base_hGraph::numHosts);
      std::vector<uint64_t> num_assigned_edges_perhost(base_hGraph::numHosts);
      num_total_edges_to_receive = 0; 

      /***Finding maximum owned nodes across hosts, for padding numOutgoingEdges****/
      uint32_t maxOwnedNodes = 0;
      for(uint32_t i = 0; i < base_hGraph::numHosts; ++i){
        uint64_t tmp = (gid2host[i].second - gid2host[i].first);
        if(maxOwnedNodes < tmp)
          maxOwnedNodes = tmp;
      }
      for(uint32_t i = 0; i < base_hGraph::numHosts; ++i){
        numOutgoingEdges[i].assign(maxOwnedNodes, 0);
        num_assigned_edges_perhost[i] = 0;
      }

      Galois::Timer timer;
      timer.start();
      g.reset_seek_counters();
      base_hGraph::totalOwnedNodes = gid2host[base_hGraph::id].second - gid2host[base_hGraph::id].first;
      uint64_t globalOffset = gid2host[base_hGraph::id].first;
      auto ee_end = g.edge_begin(gid2host[base_hGraph::id].first);
      for(auto src = gid2host[base_hGraph::id].first; src != gid2host[base_hGraph::id].second; ++src){
        auto ee = ee_end;
        ee_end = g.edge_end(src);
        auto num_edges = std::distance(ee, ee_end);
        if(num_edges > VCutThreshold){
          //Assign edges for high degree nodes to the destination
          for(; ee != ee_end; ++ee){
            auto gdst = g.getEdgeDst(ee);
            auto h = find_hostID(gdst);
            numOutgoingEdges[h][src - globalOffset]++;
            num_assigned_edges_perhost[h]++;
          }
        }
        else{
          //keep all edges with the source node
          for(; ee != ee_end; ++ee){
            numOutgoingEdges[base_hGraph::id][src - globalOffset]++;
            num_assigned_edges_perhost[base_hGraph::id]++;
            auto gdst = g.getEdgeDst(ee);
            if(!isOwned(gdst))
              ghosts[gdst] = true;
          }
        }
      }
      timer.stop();
      fprintf(stderr, "[%u] Edge inspection time : %f seconds to read %lu bytes in %lu seeks\n", base_hGraph::id, timer.get_usec()/1000000.0f, g.num_bytes_read(), g.num_seeks());

      uint64_t check_numEdges = 0;
      for(uint32_t h = 0; h < base_hGraph::numHosts; ++h){
        //std::cout << "from : " << base_hGraph::id << " to : " << h << " : edges assigned : " << num_assigned_edges_perhost[h] << "\n";
        check_numEdges += num_assigned_edges_perhost[h];
      }

      assert(check_numEdges == numEdges_distribute);

      /****** Exchange numOutgoingEdges sets *********/
      //send and clear assigned_edges_perhost to receive from other hosts
      for (unsigned x = 0; x < net.Num; ++x) {
        if(x == base_hGraph::id) continue;
        Galois::Runtime::SendBuffer b;
        Galois::Runtime::gSerialize(b, num_assigned_edges_perhost[x]);
        Galois::Runtime::gSerialize(b, numOutgoingEdges[x]);
        net.sendTagged(x, Galois::Runtime::evilPhase, b);
        //std::cerr << "sending size  : " <<  numOutgoingEdges[x].size() << "\n";
      }

        //std::cerr << " SENT \n";
      net.flush();

      //receive
      for (unsigned x = 0; x < net.Num; ++x) {
        if(x == base_hGraph::id) continue;

        decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
        do {
          net.handleReceives();
          p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
        } while(!p);

        uint64_t num_edges_from_host = 0;
        Galois::Runtime::gDeserialize(p->second, num_edges_from_host);
        Galois::Runtime::gDeserialize(p->second, numOutgoingEdges[p->first]);
        //std::cerr << "num_edges_from_host : " << num_edges_from_host << " Size : " << numOutgoingEdges[p->first].size()<<"\n";
        num_total_edges_to_receive += num_edges_from_host;
      }
      ++Galois::Runtime::evilPhase;

      numNodes = 0;
      numEdges = 0;
      localToGlobalVector.reserve(gid2host[base_hGraph::id].second - gid2host[base_hGraph::id].first);
      globalToLocalMap.reserve(gid2host[base_hGraph::id].second - gid2host[base_hGraph::id].first);
      uint64_t src = 0;
      for(uint32_t i = 0; i < base_hGraph::numHosts; ++i){
        for(unsigned j = 0; j < numOutgoingEdges[i].size(); ++j){

          bool createNode = false;
          if(numOutgoingEdges[i][j] > 0){
            createNode = true;
            numEdges += numOutgoingEdges[i][j];
          } else if (isOwned(src)){
            createNode = true;
          }
          if(createNode){
            localToGlobalVector.push_back(src);
            globalToLocalMap[src] = numNodes++;
            prefixSumOfEdges.push_back(numEdges);
            if(!isOwned(src))
              ghosts[src] = true;
          } else if (ghosts[src]){
            localToGlobalVector.push_back(src);
            globalToLocalMap[src] = numNodes++;
            prefixSumOfEdges.push_back(numEdges);
          }
          ++src;
        }
      }
      for (uint64_t x = 0; x < g.size(); ++x){
        if (ghosts[x] && !isOwned(x)){
          auto h = find_hostID(x);
          mirrorNodes[h].push_back(x);
        }
      }
      fprintf(stderr, "[%u] Resident nodes : %u , Resident edges : %lu\n", base_hGraph::id, numNodes, numEdges);

#if 0
      if(base_hGraph::id == 0){
        std::cout << "\n\n";
        for(auto i : mirrorNodes[1]){
          std::cout << "HHMIRROR : : " << i << "\t";
        }
      }
#endif
    }


    //Edge type is not void.
    template<typename GraphTy, typename std::enable_if<!std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
      void assign_load_send_edges(GraphTy& graph, Galois::Graph::OfflineGraph& g, uint64_t numEdges_distribute, uint32_t VCutThreshold){

        std::vector<std::vector<uint64_t>> gdst_vec(base_hGraph::numHosts);
        std::vector<std::vector<typename GraphTy::edge_data_type>> gdata_vec(base_hGraph::numHosts);
        auto& net = Galois::Runtime::getSystemNetworkInterface();

        auto ee_end = g.edge_begin(gid2host[base_hGraph::id].first);
        //Go over assigned nodes and distribute edges.
        for(auto src = gid2host[base_hGraph::id].first; src != gid2host[base_hGraph::id].second; ++src){
          auto ee = ee_end;
          ee_end = g.edge_end(src);

          auto num_edges = std::distance(ee, ee_end);
          for (unsigned i = 0; i < base_hGraph::numHosts; ++i) {
            gdst_vec[i].clear();
            gdata_vec[i].clear();
            //gdst_vec[i].reserve(std::distance(ii, ee));
          }
          uint32_t lsrc = 0;
          uint64_t cur = 0;
          if (isLocal(src)) {
            lsrc = G2L(src);
            cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
          }

          if(num_edges > VCutThreshold){
            //Assign edges for high degree nodes to the destination
            for(; ee != ee_end; ++ee){
              auto gdst = g.getEdgeDst(ee);
              auto gdata = g.getEdgeData<typename GraphTy::edge_data_type>(ee);
              auto h = find_hostID(gdst);
              gdst_vec[h].push_back(gdst);
              gdata_vec[h].push_back(gdata);
            }
          }
          else{
            //keep all edges with the source node
            for(; ee != ee_end; ++ee){
              auto gdst = g.getEdgeDst(ee);
              auto gdata = g.getEdgeData<typename GraphTy::edge_data_type>(ee);
              assert(isLocal(src));
              uint32_t ldst = G2L(gdst);
              graph.constructEdge(cur++, ldst, gdata);
            }
          }

          //construct edges for nodes with greater than threashold edges but assigned to local host
          uint32_t i = 0;
          for(uint64_t gdst : gdst_vec[base_hGraph::id]){
              uint32_t ldst = G2L(gdst);
              auto gdata = gdata_vec[base_hGraph::id][i++];
              graph.constructEdge(cur++, ldst, gdata);
          }

          //send all edges for one source node 
          for(uint32_t h = 0; h < base_hGraph::numHosts; ++h){
            if(h == base_hGraph::id) continue;
            if(gdst_vec[h].size()){
              Galois::Runtime::SendBuffer b;
              Galois::Runtime::gSerialize(b, src, gdst_vec[h], gdata_vec[h]);
              net.sendTagged(h, Galois::Runtime::evilPhase, b);
            }
          }
          /*** All the outgoing edges for this src are constructed ***/
          if (isLocal(src)) {
            assert(cur == (*graph.edge_end(lsrc)));
          }
        }
        net.flush();
      }


    //Edge type is void.
    template<typename GraphTy, typename std::enable_if<std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
      void assign_load_send_edges(GraphTy& graph, Galois::Graph::OfflineGraph& g, uint64_t numEdges_distribute, uint32_t VCutThreshold){

        std::vector<std::vector<uint64_t>> gdst_vec(base_hGraph::numHosts);
        auto& net = Galois::Runtime::getSystemNetworkInterface();

        auto ee_end = g.edge_begin(gid2host[base_hGraph::id].first);
        //Go over assigned nodes and distribute edges.
        for(auto src = gid2host[base_hGraph::id].first; src != gid2host[base_hGraph::id].second; ++src){
          auto ee = ee_end;
          ee_end = g.edge_end(src);

          auto num_edges = std::distance(ee, ee_end);
          for (unsigned i = 0; i < base_hGraph::numHosts; ++i) {
            gdst_vec[i].clear();
            //gdst_vec[i].reserve(std::distance(ii, ee));
          }
          uint32_t lsrc = 0;
          uint64_t cur = 0;

          if (isLocal(src)) {
              lsrc = G2L(src);
              cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
            }
          if(num_edges > VCutThreshold){
            //Assign edges for high degree nodes to the destination
            for(; ee != ee_end; ++ee){
              auto gdst = g.getEdgeDst(ee);
              auto h = find_hostID(gdst);
              gdst_vec[h].push_back(gdst);
            }
          }
          else{
            //keep all edges with the source node
            for(; ee != ee_end; ++ee){
              auto gdst = g.getEdgeDst(ee);
              assert(isLocal(src));
              uint32_t ldst = G2L(gdst);
              graph.constructEdge(cur++, ldst);
            }
            if (isLocal(src)) {
            assert(cur == (*graph.edge_end(lsrc)));
          }

          }
          //construct edges for nodes with greater than threashold edges but assigned to local host
          for(uint64_t gdst : gdst_vec[base_hGraph::id]){
              uint32_t ldst = G2L(gdst);
              graph.constructEdge(cur++, ldst);
          }

          //send if reached the batch limit
          for(uint32_t h = 0; h < base_hGraph::numHosts; ++h){
            if(h == base_hGraph::id) continue;
            if(gdst_vec[h].size()){
              Galois::Runtime::SendBuffer b;
              Galois::Runtime::gSerialize(b, src, gdst_vec[h]);
              net.sendTagged(h, Galois::Runtime::evilPhase, b);
            }
          }
        }

        net.flush();
      }


    template<typename GraphTy>
    void receive_edges(GraphTy& graph){
      Galois::StatTimer StatTimer_exchange_edges("RECEIVE_EDGES_TIME");
      auto& net = Galois::Runtime::getSystemNetworkInterface();

      //receive the edges from other hosts
      while(num_total_edges_to_receive){

        decltype(net.recieveTagged(Galois::Runtime::evilPhase, nullptr)) p;
        net.handleReceives();
        p = net.recieveTagged(Galois::Runtime::evilPhase, nullptr);
        if(p){
          std::vector<uint64_t> _gdst_vec;
          uint64_t _src;
          Galois::Runtime::gDeserialize(p->second, _src, _gdst_vec);
          num_total_edges_to_receive -= _gdst_vec.size();
          assert(isLocal(_src));
          uint32_t lsrc = G2L(_src);
          uint64_t cur = *graph.edge_begin(lsrc, Galois::MethodFlag::UNPROTECTED);
          uint64_t cur_end = *graph.edge_end(lsrc);
          assert((cur_end - cur) == _gdst_vec.size());
          deserializeEdges(graph, p->second, _gdst_vec, cur, cur_end);
        }
      }
    }

  template<typename GraphTy, typename std::enable_if<!std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void deserializeEdges(GraphTy& graph, Galois::Runtime::RecvBuffer& b, 
      std::vector<uint64_t>& gdst_vec, uint64_t& cur, uint64_t& cur_end) {
    std::vector<typename GraphTy::edge_data_type> gdata_vec;
    Galois::Runtime::gDeserialize(b, gdata_vec);
    uint64_t i = 0;
    while (cur < cur_end) {
      auto gdata = gdata_vec[i];
      uint64_t gdst = gdst_vec[i++];
      uint32_t ldst = G2L(gdst);
      graph.constructEdge(cur++, ldst, gdata);
    }
  }

  template<typename GraphTy, typename std::enable_if<std::is_void<typename GraphTy::edge_data_type>::value>::type* = nullptr>
  void deserializeEdges(GraphTy& graph, Galois::Runtime::RecvBuffer& b, 
      std::vector<uint64_t>& gdst_vec, uint64_t& cur, uint64_t& cur_end) {
    uint64_t i = 0;
    while (cur < cur_end) {
      uint64_t gdst = gdst_vec[i++];
      uint32_t ldst = G2L(gdst);
      graph.constructEdge(cur++, ldst);
    }
  }
    uint32_t find_hostID(uint64_t gid){
      for(uint32_t h = 0; h < base_hGraph::numHosts; ++h){
        if(gid >= gid2host[h].first && gid < gid2host[h].second)
          return h;
        else if(isBipartite && (gid >= gid2host_withoutEdges[h].first && gid < gid2host_withoutEdges[h].second))
          return h;
        else
          continue;
      }
      return -1;
    }

#if 0
    uint32_t G2L(const uint64_t gid) const {
      return GlobalVec_map.at(gid);
    }
#endif

#if 0
    uint32_t G2L(uint64_t gid) const {
      //we can assume that GID exits and is unique. Index is localID since it is sorted.
      uint32_t found_index  = ~0;
      for(auto i : hostNodes){
        if(i.first != ~0){
          auto iter = std::lower_bound(GlobalVec.begin() + i.first, GlobalVec.begin() + i.second, gid);
          if(*iter == gid)
            return (iter - GlobalVec.begin());
        }
      }
      abort();
    }
#endif

#if 0
    uint64_t L2G(uint32_t lid) const {
      return GlobalVec[lid];
    }
#endif

#if 0
    void fill_mirrorNodes(std::vector<std::vector<size_t>>& mirrorNodes){

      std::vector<std::vector<uint64_t>> GlobalVec_perHost(base_hGraph::numHosts);
      std::vector<std::vector<uint32_t>> OwnerVec_perHost(base_hGraph::numHosts);
      std::vector<uint64_t> nodesOnHost_vec;


      fill_edge_map<EdgeTy>(nodesOnHost_vec);
      //Fill GlobalVec_perHost and mirrorNodes vetors using assigned_edges_perhost.

      //Isolated nodes
      for(auto n = gid2host[base_hGraph::id].first; n < gid2host[base_hGraph::id].second; ++n){
        nodesOnHost_vec.push_back(n);
      }

      if(isBipartite){
        // Isolated nodes for bipartite graphs nodes without edges.
        for(auto n = gid2host_withoutEdges[base_hGraph::id].first; n < gid2host_withoutEdges[base_hGraph::id].second; ++n){
          nodesOnHost_vec.push_back(n);
        }
      }

      // Only keep unique node ids in vector
      std::sort(nodesOnHost_vec.begin(), nodesOnHost_vec.end());
      nodesOnHost_vec.erase(std::unique(nodesOnHost_vec.begin(), nodesOnHost_vec.end()), nodesOnHost_vec.end());

      numNodes = nodesOnHost_vec.size();

      for(auto i : nodesOnHost_vec){
        // Need to add both source and destination nodes
        // Source : i_pair.first
        // Destination : i_pair.second
        auto owner = find_hostID(i);
        GlobalVec_perHost[owner].push_back(i);
        OwnerVec_perHost[owner].push_back(owner);
        mirrorNodes[owner].push_back(i);
      }

      //release memory held by nodesOnHost_vec
      std::vector<uint64_t>().swap(nodesOnHost_vec);

      hostNodes.resize(base_hGraph::numHosts);
      uint32_t counter = 0;
      for(uint32_t i = 0; i < base_hGraph::numHosts; i++){
        if(GlobalVec_perHost[i].size() > 0){
          hostNodes[i] = std::make_pair(counter, GlobalVec_perHost[i].size() + counter);
          counter += GlobalVec_perHost[i].size();
        }
        else {
          hostNodes[i] = std::make_pair(~0, ~0);
        }
        std::sort(GlobalVec_perHost[i].begin(), GlobalVec_perHost[i].end());
        std::sort(OwnerVec_perHost[i].begin(), OwnerVec_perHost[i].end());
      }

      base_hGraph::totalOwnedNodes = GlobalVec_perHost[base_hGraph::id].size();
      auto iter_insert = GlobalVec.begin();
      for(auto v : GlobalVec_perHost){
        for(auto j : v){
          GlobalVec.push_back(j);
        }
      }
      std::vector<std::vector<uint64_t>>().swap(GlobalVec_perHost);

      //trasfer to unordered_map
      uint32_t local_id = 0;
      for(auto v : GlobalVec){
        GlobalVec_map[v] = local_id;
        ++local_id;
      }


      OwnerVec.reserve(counter);
      iter_insert = OwnerVec.begin();
      for(auto v : OwnerVec_perHost){
        for(auto j : v){
          OwnerVec.push_back(j);
        }
      }
      std::vector<std::vector<uint32_t>>().swap(OwnerVec_perHost);
    }
#endif

    bool is_vertex_cut() const{
      return true;
    }

    /*
   * Returns the total nodes : master + slaves created on the local host.
   */
    uint64_t get_local_total_nodes() const {
      return (base_hGraph::numOwned);
    }

    void reset_bitset(typename base_hGraph::SyncType syncType, void (*bitset_reset_range)(size_t, size_t)) const {
      size_t first_owned = G2L(gid2host[base_hGraph::id].first);
      size_t last_owned = G2L(gid2host[base_hGraph::id].second - 1);
      assert(first_owned <= last_owned);
      assert((last_owned - first_owned + 1) == base_hGraph::totalOwnedNodes);
      if (syncType == base_hGraph::syncBroadcast) { // reset masters
        bitset_reset_range(first_owned, last_owned);
      } else { // reset mirrors
        assert(syncType == base_hGraph::syncReduce);
        if (first_owned > 0) {
          bitset_reset_range(0, first_owned - 1);
        }
        if (last_owned < (numNodes - 1)) {
          bitset_reset_range(last_owned + 1, numNodes - 1);
        }
      }
    }

    void print_string(std::string s){
      std::stringstream ss_cout;
      ss_cout << base_hGraph::id << s << "\n";
      std::cerr << ss_cout.str();
    }

#if 0
    void save_meta_file(std::string file_name_prefix) const {
      std::string meta_file_str = file_name_prefix +".gr.META." + std::to_string(base_hGraph::id) + ".OF." + std::to_string(base_hGraph::numHosts);
      std::string tmp_meta_file_str = file_name_prefix +".gr.TMP." + std::to_string(base_hGraph::id) + ".OF." + std::to_string(base_hGraph::numHosts);
      std::ofstream meta_file(meta_file_str.c_str());
      std::ofstream tmp_file;
      tmp_file.open(tmp_meta_file_str.c_str());

      size_t num_nodes = (size_t)base_hGraph::numOwned;
      std::cerr << base_hGraph::id << "  NUMNODES  : " <<  num_nodes << "\n";
      meta_file.write(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));
      for(size_t lid = 0; lid < base_hGraph::numOwned; ++lid){

      size_t gid = L2G(lid);
      size_t owner = getOwner_lid(lid);
#if 0
      //for(auto src = base_hGraph::graph.begin(), src_end = base_hGraph::graph.end(); src != src_end; ++src){
        size_t lid = (size_t)(*src);
        size_t gid = (size_t)L2G(*src);
        size_t owner = (size_t)getOwner_lid(lid);
#endif
        meta_file.write(reinterpret_cast<char*>(&gid), sizeof(gid));
        meta_file.write(reinterpret_cast<char*>(&lid), sizeof(lid));
        meta_file.write(reinterpret_cast<char*>(&owner), sizeof(owner));

        tmp_file << gid << " " << lid << " " << owner << "\n";
      }
    }
#endif

};

