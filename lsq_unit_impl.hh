/*
 * Copyright (c) 2010-2014, 2017 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Kevin Lim
 *          Korey Sewell
 */

#ifndef __CPU_O3_LSQ_UNIT_IMPL_HH__
#define __CPU_O3_LSQ_UNIT_IMPL_HH__

#include "arch/generic/debugfaults.hh"
#include "arch/locked_mem.hh"
#include "base/str.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/lsq_unit.hh"
#include "debug/Activity.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "debug/CleanupTime.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/backtrace.hh"
#include "mem/ruby/common/Address.hh"

#define SPEC_WINDOW 200
#define BUFFER_CYCLES 20

template<class Impl>
LSQUnit<Impl>::WritebackEvent::WritebackEvent(DynInstPtr &_inst, PacketPtr _pkt,
                                              LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
{
}

template<class Impl>
void
LSQUnit<Impl>::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    if (pkt->senderState)
        delete pkt->senderState;

    if (!pkt->isValidate() && !pkt->isExpose()){
        delete pkt->req;
    }
    delete pkt;
}

template<class Impl>
const char *
LSQUnit<Impl>::WritebackEvent::description() const
{
    return "Store writeback";
}


// [InvisiSpec] This function deals with
// acknowledge response to memory read/write
template<class Impl>
void
LSQUnit<Impl>::completeDataAccess(PacketPtr pkt)
{
  cpu->wakeCPU();
  iewStage->activityThisCycle();
  
  LSQSenderState *state = dynamic_cast<LSQSenderState *>(pkt->senderState);
  DynInstPtr inst = state->inst;
  DPRINTF(IEW, "Writeback event [sn:%lli].\n", inst->seqNum);
  DPRINTF(Activity, "Activity: Writeback event [sn:%lli].\n", inst->seqNum);

  if(pkt->isCleanup()){
    if(pkt->isResponse()){
      processCleanup(inst,pkt);
      //Delete State
      delete state;
      return;
    } else{
      assert(0); //Not sure how to handle cleanup requests from cache -> CPU here
    }
      
  }
    
  if (state->cacheBlocked) {
    // This is the first half of a previous split load,
    // where the 2nd half blocked, ignore this response
    DPRINTF(IEW, "[sn:%lli]: Response from first half of earlier "
            "blocked split load recieved. Ignoring.\n", inst->seqNum);
    delete state;
    return;
  }

  if( state->isSplit && pkt->isFirst()){
    ++lsqLoadsSplit;
  }
       
  // need to update hit info for corresponding instruction
  if (pkt->isL1Hit() && pkt->isSpec() && pkt->isRead()){
    if (state->isSplit && ! pkt->isFirst()){
      inst->setL1HitHigh();
    } else {
      inst->setL1HitLow();
    }
  } else if (!pkt->isSpec()) {
    setSpecBuffState(pkt->req);
  }

  //[CleanupCache] Update L1 Hit Status.     
  if(pkt->isRead()){
    inst->setLoadReturned();

    if (pkt->isL1Hit()){      
      inst->setL1HitCC();
      inst->EvictedLineAddrL1 = ((Addr) -1);
    } else {
      //Means L1Miss
      inst->EvictedLineAddrL1 = pkt->EvictedLineAddr;
      if(pkt->isL2Hit()){
        inst->setL2HitCC();
      } else if (pkt->isL2Miss()) {
        inst->setL2MissCC();
      } // else{
        //   assert(0);
        // }
      
    }
    inst->MemCompletionTime = pkt->MemCompletionTime;
    DPRINTF(LSQUnit,"***LoadReturned:%d to LSQ for Addr:%#x, sn:%d, L1-Hit %d, L2-Hit %d, L2-Miss %d, with EvictLineAddrL1:%#x, at Time: %llu\n",inst->isLoadReturned(), inst->physEffAddrLow, inst->seqNum, inst->isL1HitCC(),inst->isL2HitCC(),inst->isL2MissCC(), inst->EvictedLineAddrL1, inst->MemCompletionTime);
    
  } else {
    inst->MemCompletionTime = pkt->MemCompletionTime;
    DPRINTF(LSQUnit,"***StoreReturned:%d to LSQ for Addr:%#x, sn:%d, L1-Hit %d, L2-Hit %d, L2-Miss %d, with EvictLineAddrL1:%#x, at Time: %llu\n",inst->isLoadReturned(), inst->physEffAddrLow, inst->seqNum, inst->isL1HitCC(),inst->isL2HitCC(),inst->isL2MissCC(), inst->EvictedLineAddrL1, inst->MemCompletionTime);

  }


  // If this is a split access, wait until all packets are received.
  if (TheISA::HasUnalignedMemAcc && !state->complete()) {
    // Not the good place, but we need to fix the memory leakage
    if (pkt->isExpose() || pkt->isValidate()){
      assert(!inst->needDeletePostReq());
      assert(!pkt->isInvalidate());
      delete pkt->req;
    }
    return;
  }

  //If reordered, then include a way to delay.
  if(profile_reordered_loads(inst) && internalInterferenceHit
     && !inst->isSquashed()){
    DPRINTF(LSQUnit, "Pushing into Delayed_Load_Queue: sn:%llu to be poped at %llu Epoch. Size=%d \n",
            inst->seqNum, inst->loadDelayedReturnEpoch,d_loadQueue.size());
    d_loadQueue.push(pkt);
    pkt->delayedDelete = true;
    assert(!state->isSplit);
    return;
  }
  //Go ahead with the completeDataAccess
  else {    
    completeDataAccess_Phase2(pkt);    
  }
}

template <class Impl>
LSQUnit<Impl>::LSQUnit()
  : loads(0), loadsToVLD(0), loadsInflightCleanupIssued(0), loadsExecutedCleanupIssued(0), cleanupAcksReceived(0),
          stores(0), storesToWB(0), cacheBlockMask(0), stalled(false),
      isStoreBlocked(false), isValidationBlocked(false), storeInFlight(false), hasPendingPkt(false),
      pendingPkt(nullptr)
{
}

template<class Impl>
void
LSQUnit<Impl>::init(O3CPU *cpu_ptr, IEW *iew_ptr, DerivO3CPUParams *params,
        LSQ *lsq_ptr, unsigned maxLQEntries, unsigned maxSQEntries,
        unsigned id)
{
    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    lsqID = id;

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",id);

    // Add 1 for the sentinel entry (they are circular queues).
    LQEntries = maxLQEntries + 1;
    SQEntries = maxSQEntries + 1;

    //Due to uint8_t index in LSQSenderState
    assert(LQEntries <= 256);
    assert(SQEntries <= 256);

    loadQueue.resize(LQEntries);
    storeQueue.resize(SQEntries);

    depCheckShift = params->LSQDepCheckShift;
    checkLoads = params->LSQCheckLoads;
    cacheStorePorts = params->cacheStorePorts;

    // According to the scheme, we need to define actions as follows.
    // loadInExec: if False, no packets are sent in execution stage;
    //             if True, send either readReq or readSpecReq
    // isInvisibleSpec: if True, send readSpecReq in execution statge;
    //                  if False, send readReq
    // needsTSO: if True, squash read on receiving invalidations, and only allow one outstanding write at a time;
    //           if False, no squash on receiving invalidaiton, and allow multiple outstanding writes.
    // isConservative: if True, react after all preceding instructions complete/no exception;
    //                 if False, react only after all preceding stores/brancehs complete
    const std::string scheme = params->simulateScheme;
    if (scheme.compare("UnsafeBaseline")==0){
        loadInExec = true;
        isInvisibleSpec = false; // send real request
        isFuturistic = false; // not relevant in unsafe mode.
    }else if (scheme.compare("FuturisticSafeFence")==0){
        // "LFENCE" before every load
        loadInExec = false;
        isInvisibleSpec = false; // not used since loadInExec is false
        isFuturistic = true; // send readReq at head of ROB
    }else if (scheme.compare("FuturisticSafeInvisibleSpec")==0){
        // only make load visible when all preceding instructions
        // complete and no exception
        loadInExec = true;
        isInvisibleSpec = true; // send request but not change cache state
        isFuturistic = true; // conservative condition to send validations
    }else if (scheme.compare("SpectreSafeFence")==0){
        // "LFENCE" after every branch
        loadInExec = false;
        isInvisibleSpec = false; // not used since loadInExec is false
        isFuturistic = false; // commit when preceding branches are resolved
    }else if (scheme.compare("SpectreSafeInvisibleSpec")==0){
        // make load visible when all preceiding branches are resolved
        loadInExec = true;
        isInvisibleSpec = true; // send request but not change cache state
        isFuturistic = false; // only deal with spectre attacks
    }else {
        cprintf("ERROR: unsupported simulation scheme: %s!\n", scheme);
        exit(1);
    }
    needsTSO = params->needsTSO;
    allowSpecBuffHit = params->allowSpecBuffHit;

    //[CleanupCache]
    cleanupLQExecd_on_Squash = params->cleanupLQExecd_on_Squash;
    cleanupLQInflight_on_Squash = params->cleanupLQInflight_on_Squash;
    cleanupL2Inv  = params->L2inv_on_Squash;

    internalInterferenceHit = params->InternalInterferenceHit;
    
    // cprintf("Info: simulation uses scheme: %s; "
    //         "needsTSO=%d; allowSpecBuffHit=%d\n",
    //             scheme, needsTSO, allowSpecBuffHit);

    //Init CleanupTime hist.
    lsqCleanupSquashTimeHist.init(100);

    //Init loadSpecWindow
    specWindowLoadsHist.init(100);
    spectreWindowLoadsHist.init(100);
    
    // [mengjia] end of setting configuration variables
    resetState();
   
}


template<class Impl>
void
LSQUnit<Impl>::resetState()
{
    loads = stores = loadsToVLD = storesToWB = 0;

    loadHead = loadTail = 0;

    storeHead = storeWBIdx = storeTail = 0;

    usedStorePorts = 0;

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    cacheBlockMask = ~(cpu->cacheLineSize() - 1);
    //[CleanupCache]
    loadsInflightCleanupIssued =  loadsExecutedCleanupIssued = cleanupAcksReceived = skippedCleanupReqs = 0;
    squash_in_progress = false;
    squashbuffer_used = false;
    squashbuffer_empty = false;
    squashbuffer_emptying_started = false;

    squashfrom_seqnum = -1; //really large number

    //Reset Histogram tracking cleanup time.
    lsqCleanupSquashTimeHist.reset();
    specWindowLoadsHist.reset();
    spectreWindowLoadsHist.reset();
}

template<class Impl>
std::string
LSQUnit<Impl>::name() const
{
    if (Impl::MaxThreads == 1) {
      return iewStage->name() + ".lsq";
    } else {
      return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

template<class Impl>
void
LSQUnit<Impl>::regStats()
{
    lsqForwLoads
        .name(name() + ".forwLoads")
        .desc("Number of loads that had data forwarded from stores");

    invAddrLoads
        .name(name() + ".invAddrLoads")
        .desc("Number of loads ignored due to an invalid address");

    lsqSquashEvents
      .name(name() + ".squashEvents")
      .desc("Number of Squash Events in Processor, that trigger Cleanup");

    lsqCommitLoads
      .name(name() + ".commitLoads")
      .desc("Number of Loads Committed");

    lsqCommitSpecHitLoads
      .name(name() + ".commitLoadsHitSpec")
      .desc("Number of Loads Committed that Hit on Speculated Reordered Loads");
    
    lsqSpecHitLoadsGainedCycles
      .name(name() + ".commitLoadsHitSpecGainedCycles")
      .desc("Number of Cycles Gained by Loads Committed that Hit on Speculated Reordered Loads");

    specWindowExtensionMsgs
      .name(name() + ".BWspecWindowExtensionMsgs")
      .desc("Number of Messages Sent to L2 to extend the Spec Window every 200 cycles");

    specWindowL2StorageExtra
      .name(name() + ".StorageSpecWindowInstallTable")
      .desc("Number of Lines That are Extra (L2 Installs but Still Stored in Table Speculatively)");
 
    spectreWindowLoadsHist
      .init(100)
    .name(name() + ".spectreWindowLoadsCyclesHist")
    .desc("")
    .flags(Stats::nozero | Stats::pdf | Stats::oneline);
    
  specWindowLoadsHist
    .init(100)
    .name(name() + ".specTotalWindowLoadsCyclesHist")
    .desc("")
    .flags(Stats::nozero | Stats::pdf | Stats::oneline);

  lsqCleanupSquashTimeHist
    .init(100)
    .name(name() + ".cleanupSquashTimeHist")
    .desc("")
    .flags(Stats::nozero | Stats::pdf | Stats::oneline);

  lsqCleanupSquashTimeCycles
    .name(name() + ".cleanupSquashTimeCycles")
    .desc("Time taken to execute cleanup squash (in cycles)");

  lsqCleanupSquashBufferWaitingTimeCycles
    .name(name() + ".cleanupSquashTimeWaitingCycles")
    .desc("Time taken waiting for cleanup squash to begin while correct path executes (in cycles)");

  lsqActCleanupSquashTimeHist
    .init(100)
    .name(name() + ".cleanupSquashTimeActHist")
    .desc("")
    .flags(Stats::nozero | Stats::pdf | Stats::oneline);

  lsqActCleanupExecL1MissHist
    .init(100)
    .name(name() + ".cleanupSquashExecL1MissHist")
    .desc("")
    .flags(Stats::nozero | Stats::pdf | Stats::oneline);
    
  lsqCleanupSquashes
    .name(name() + ".cleanupSquashNum")
    .desc("Number of Cleanup Squashes");
      
  lsqSquashedLoadsTot
    .name(name() + ".squashedLoadsTot")
        .desc("Number of loads squashed");

    lsqSquashedLoadsNotIssued
      .name(name() + ".squashedLoadsNotIssued")
      .desc("Number of loads squashed before request sent to memory system");

    lsqSquashedLoadsExec_Tot
      .name(name() + ".squashedLoadsExec_Tot")
      .desc("Number of loads squashed after data received");

    lsqSquashedLoadsExec_NoHitWhere
      .name(name() + ".squashedLoadsExec_NoHitWhere")
      .desc("Number of loads squashed after data received but no HitWhere, returned through writeCallback on a likely RMW ");

    lsqSquashedLoadsExec_L1Hits
      .name(name() + ".squashedLoadsExec_L1Hits")
      .desc("Number of loads (L1Hits) squashed after data received");

    lsqSquashedLoadsExec_L2Hits
      .name(name() + ".squashedLoadsExec_L2Hits")
      .desc("Number of loads (L2Hits) squashed after data received");

    lsqSquashedLoadsExec_L2Miss
      .name(name() + ".squashedLoadsExec_L2Miss")
      .desc("Number of loads (L2Miss) squashed after data received");

    lsqSquashedLoadsInflight_Tot
      .name(name() + ".squashedLoadsInflight_Tot")
      .desc("Number of loads squashed while in-flight");

    lsqSquashedLoadsInflight_L1Hits
      .name(name() + ".squashedLoadsInflight_L1Hits")
      .desc("Number of loads squashed while in-flight - L1 Hits");

    lsqSquashedLoadsInflight_L2Hits
      .name(name() + ".squashedLoadsInflight_L2Hits")
      .desc("Number of loads squashed while in-flight L2 Hits");
    
    lsqSquashedLoadsInflight_L2Miss
      .name(name() + ".squashedLoadsInflight_L2Miss")
      .desc("Number of loads squashed while in-flight L2 Miss");

    lsqSquashedLoadsInflight_NoHitWhere
      .name(name() + ".squashedLoadsInflight_NoHitWhere")
      .desc("Number of loads squashed inflight with no HitWhere, returned through writeCallback on a likely RMW ");

    lsqSquashedLoadsSplitReq
      .name(name() + ".squashedLoadsSplitReq")
      .desc("Number of loads (with Split Requests) squashed");
    
    lsqLoadsSplit
      .name(name() + ".LoadsSplit")
      .desc("Number of loads that are Split");

    lsqIgnoredResponses
        .name(name() + ".ignoredResponses")
        .desc("Number of memory responses ignored because the instruction is squashed");

    lsqMemOrderViolation
        .name(name() + ".memOrderViolation")
        .desc("Number of memory ordering violations");

    lsqSquashedStores
        .name(name() + ".squashedStores")
        .desc("Number of stores squashed");

    invAddrSwpfs
        .name(name() + ".invAddrSwpfs")
        .desc("Number of software prefetches ignored due to an invalid address");

    lsqBlockedLoads
        .name(name() + ".blockedLoads")
        .desc("Number of blocked loads due to partial load-store forwarding");

    lsqRescheduledLoads
        .name(name() + ".rescheduledLoads")
        .desc("Number of loads that were rescheduled");

    lsqCacheBlocked
        .name(name() + ".cacheBlocked")
        .desc("Number of times an access to memory failed due to the cache being blocked");

    specBuffHits
        .name(name() + ".specBuffHits")
        .desc("Number of times an access hits in speculative buffer");

    specBuffMisses
        .name(name() + ".specBuffMisses")
        .desc("Number of times an access misses in speculative buffer");

    numValidates
        .name(name() + ".numValidates")
        .desc("Number of validates sent to cache");

    numExposes
        .name(name() + ".numExposes")
        .desc("Number of exposes sent to cache");

    numConvertedExposes
        .name(name() + ".numConvertedExposes")
        .desc("Number of exposes converted from validation");
}

template<class Impl>
void
LSQUnit<Impl>::setDcachePort(MasterPort *dcache_port)
{
    dcachePort = dcache_port;
}

template<class Impl>
void
LSQUnit<Impl>::clearLQ()
{
    loadQueue.clear();
}

template<class Impl>
void
LSQUnit<Impl>::clearSQ()
{
    storeQueue.clear();
}

template<class Impl>
void
LSQUnit<Impl>::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.size(); ++i)
        assert(!loadQueue[i]);

    assert(storesToWB == 0);
    assert(loadsToVLD == 0);
    assert(!retryPkt);

    //[CleanupCache]
    assert(!squash_in_progress);
}

template<class Impl>
void
LSQUnit<Impl>::takeOverFrom()
{
    resetState();
}

template<class Impl>
void
LSQUnit<Impl>::resizeLQ(unsigned size)
{
    unsigned size_plus_sentinel = size + 1;
    assert(size_plus_sentinel >= LQEntries);

    if (size_plus_sentinel > LQEntries) {
        while (size_plus_sentinel > loadQueue.size()) {
            DynInstPtr dummy;
            loadQueue.push_back(dummy);
            LQEntries++;
        }
    } else {
        LQEntries = size_plus_sentinel;
    }

    assert(LQEntries <= 256);
}

template<class Impl>
void
LSQUnit<Impl>::resizeSQ(unsigned size)
{
    unsigned size_plus_sentinel = size + 1;
    if (size_plus_sentinel > SQEntries) {
        while (size_plus_sentinel > storeQueue.size()) {
            SQEntry dummy;
            storeQueue.push_back(dummy);
            SQEntries++;
        }
    } else {
        SQEntries = size_plus_sentinel;
    }

    assert(SQEntries <= 256);
}

template <class Impl>
void
LSQUnit<Impl>::insert(DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

template <class Impl>
void
LSQUnit<Impl>::insertLoad(DynInstPtr &load_inst)
{
    assert((loadTail + 1) % LQEntries != loadHead);
    assert(loads < LQEntries);

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadTail, load_inst->seqNum);

    load_inst->lqIdx = loadTail;

    if (stores == 0) {
        load_inst->sqIdx = -1;
    } else {
        load_inst->sqIdx = storeTail;
    }

    loadQueue[loadTail] = load_inst;

    incrLdIdx(loadTail);

    ++loads;

}

template <class Impl>
void
LSQUnit<Impl>::insertStore(DynInstPtr &store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert((storeTail + 1) % SQEntries != storeHead);
    assert(stores < SQEntries);

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeTail, store_inst->seqNum);

    store_inst->sqIdx = storeTail;
    store_inst->lqIdx = loadTail;

    storeQueue[storeTail] = SQEntry(store_inst);

    incrStIdx(storeTail);

    ++stores;
}

// It is an empty function? why? [mengjia]
template <class Impl>
typename Impl::DynInstPtr
LSQUnit<Impl>::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeLoadEntries()
{
        //LQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n", LQEntries, loads);
        return LQEntries - loads - 1;
}

template <class Impl>
unsigned
LSQUnit<Impl>::numFreeStoreEntries()
{
        //SQ has an extra dummy entry to differentiate
        //empty/full conditions. Subtract 1 from the free entries.
        DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n", SQEntries, stores);
        return SQEntries - stores - 1;

 }

template <class Impl>
void
LSQUnit<Impl>::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    int load_idx = loadHead;
    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    // Only Invalidate packet calls checkSnoop
    assert(pkt->isInvalidate());
    for (int x = 0; x < cpu->numContexts(); x++) {
        ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        TheISA::handleLockedSnoop(tc, pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = loadQueue[load_idx];
    if (ld_inst) {
        Addr load_addr_low = ld_inst->physEffAddrLow & cacheBlockMask;
        Addr load_addr_high = ld_inst->physEffAddrHigh & cacheBlockMask;

        // Check that this snoop didn't just invalidate our lock flag
        // [InvisiSpec] also make sure the instruction has been sent out
        // otherwise, we cause unneccessary squash
        if (ld_inst->effAddrValid() && !ld_inst->fenceDelay()
                && (load_addr_low == invalidate_addr
                    || load_addr_high == invalidate_addr)
            && ld_inst->memReqFlags & Request::LLSC)
            TheISA::handleLockedSnoopHit(ld_inst.get());
    }

    // If not match any load entry, then do nothing [mengjia]
    if (load_idx == loadTail)
        return;

    incrLdIdx(load_idx);

    bool force_squash = false;

    while (load_idx != loadTail) {
        DynInstPtr ld_inst = loadQueue[load_idx];

        // [SafeSpce] check snoop violation when the load has
        // been sent out; otherwise, unneccessary squash
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()
                || ld_inst->fenceDelay()) {
            incrLdIdx(load_idx);
            continue;
        }

        Addr load_addr_low = ld_inst->physEffAddrLow & cacheBlockMask;
        Addr load_addr_high = ld_inst->physEffAddrHigh & cacheBlockMask;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] load_addr: %#x to pktAddr:%#x\n",
                    ld_inst->seqNum, load_addr_low, invalidate_addr);

        if ((load_addr_low == invalidate_addr
             || load_addr_high == invalidate_addr) || force_squash) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;

                // [InvisiSpec] in InvisiSpec, we do not need to squash
                // the load at the head of LQ,
                // as well as the one do not need validation
                if (isInvisibleSpec &&
                    (load_idx==loadHead || ld_inst->needExposeOnly())){
                    force_squash = false;
                }
                if (!pkt->isExternalEviction() && isInvisibleSpec){
                    force_squash = false;
                    ld_inst->clearL1HitHigh();
                    ld_inst->clearL1HitLow();
                }
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                //[InvisiSpec] mark the load hit invalidation
                ld_inst->hitInvalidation(true);
                if (pkt->isExternalEviction()){
                    ld_inst->hitExternalEviction(true);
                }
                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC)
                    TheISA::handleLockedSnoopHit(ld_inst.get());

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
        incrLdIdx(load_idx);
    }
    return;
}

template <class Impl>
bool
LSQUnit<Impl>::checkPrevLoadsExecuted(int req_idx)
{
    int load_idx = loadHead;
    while (load_idx != req_idx){
        if (!loadQueue[load_idx]->isExecuted()){
            // if at least on load ahead of current load
            // does not finish spec access,
            // then return false
            return false;
        }
        incrLdIdx(load_idx);
    }

    //if all executed, return true
    return true;
}

template <class Impl>
void
LSQUnit<Impl>::setSpecBuffState(RequestPtr expose_req)
{
    Addr req_eff_addr1 = expose_req->getPaddr() & cacheBlockMask;

    int load_idx = loadHead;
    while (load_idx != loadTail){
        DynInstPtr ld_inst = loadQueue[load_idx];
        if (ld_inst->effAddrValid()){

            Addr ld_eff_addr1 = ld_inst->physEffAddrLow & cacheBlockMask;
            Addr ld_eff_addr2 = ld_inst->physEffAddrHigh & cacheBlockMask;
            if (ld_eff_addr1 == req_eff_addr1){
                ld_inst->setSpecBuffObsoleteLow();
            } else if (ld_eff_addr2 == req_eff_addr1){
                ld_inst->setSpecBuffObsoleteHigh();
            }
        }
        incrLdIdx(load_idx);
    }
}


template <class Impl>
int
LSQUnit<Impl>::checkSpecBuffHit(RequestPtr req, int req_idx)
{

    Addr req_eff_addr1 = req->getPaddr() & cacheBlockMask;
    //Addr req_eff_addr2 = (req->getPaddr() + req->getSize()-1) & cacheBlockMask;
    // the req should be within the same cache line
    //assert (req_eff_addr1 == req_eff_addr2);
    assert (!loadQueue[req_idx]->isExecuted());

    int load_idx = loadHead;

    while (load_idx != loadTail){
        DynInstPtr ld_inst = loadQueue[load_idx];
        if (ld_inst->effAddrValid()){
            Addr ld_eff_addr1 = ld_inst->physEffAddrLow & cacheBlockMask;
            Addr ld_eff_addr2 = ld_inst->physEffAddrHigh & cacheBlockMask;

            if ((req_eff_addr1 == ld_eff_addr1 && ld_inst->isL1HitLow())
                || (req_eff_addr1 == ld_eff_addr2 && ld_inst->isL1HitHigh())){
                return -1;
                //already in L1, do not copy from buffer
            } else {

                if (ld_inst->isExecuted() && ld_inst->needPostFetch()
                    && !ld_inst->isSquashed() && ld_inst->fault==NoFault){
                    if (req_eff_addr1 == ld_eff_addr1 && !ld_inst->isL1HitLow()
                            && !ld_inst->isSpecBuffObsoleteLow()){
                        DPRINTF(LSQUnit, "Detected Spec Hit with inst [sn:%lli] "
                            "and [sn:%lli] (low) at address %#x\n",
                            loadQueue[req_idx]->seqNum, ld_inst->seqNum,
                            req_eff_addr1);
                        return load_idx;
                    } else if ( ld_eff_addr2 !=0  &&
                        req_eff_addr1 == ld_eff_addr2 && !ld_inst->isL1HitHigh()
                        && !ld_inst->isSpecBuffObsoleteHigh()){
                        DPRINTF(LSQUnit, "Detected Spec Hit with inst [sn:%lli] "
                            "and [sn:%lli] (high) at address %#x\n",
                            loadQueue[req_idx]->seqNum, ld_inst->seqNum,
                            req_eff_addr1);
                        return load_idx;
                    }
                }
            }
        }
        incrLdIdx(load_idx);
    }

    return -1;
}



template <class Impl>
Fault
LSQUnit<Impl>::checkViolations(int load_idx, DynInstPtr &inst)
{
    Addr inst_eff_addr1 = inst->effAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->effAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (load_idx != loadTail) {
        DynInstPtr ld_inst = loadQueue[load_idx];
        // [InvisiSpec] no need to check violation for unsent load
        // otherwise, unneccessary squash
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()
                || ld_inst->fenceDelay()) {
            incrLdIdx(load_idx);
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->effAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->effAddr + ld_inst->effSize - 1) >> depCheckShift;

        if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
            if (inst->isLoad()) {
                // If this load is to the same block as an external snoop
                // invalidate that we've observed then the load needs to be
                // squashed as it could have newer data
                if (ld_inst->hitExternalSnoop()) {
                    if (!memDepViolator ||
                            ld_inst->seqNum < memDepViolator->seqNum) {
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                "and [sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++lsqMemOrderViolation;

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with inst [sn:%lli] and "
                            "[sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }

                // Otherwise, mark the load has a possible load violation
                // and if we see a snoop before it's commited, we need to squash
                ld_inst->possibleLoadViolation(true);
                DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                        " between instructions [sn:%lli] and [sn:%lli]\n",
                        inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                        "[sn:%lli] at address %#x\n",
                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                memDepViolator = ld_inst;

                ++lsqMemOrderViolation;

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        incrLdIdx(load_idx);
    }
    return NoFault;
}




template <class Impl>
Fault
LSQUnit<Impl>::executeLoad(DynInstPtr &inst)
{
    using namespace TheISA;
    // Execute a specific load.
    Fault load_fault = NoFault;

    DPRINTF(LSQUnit, "Executing load PC %s, [sn:%lli]\n",
            inst->pcState(), inst->seqNum);

    assert(!inst->isSquashed());

    // use ISA interface to generate correct access request
    // initiateAcc is implemented in dyn_inst_impl.hh
    // The interface calls corresponding ISA defined function
    // check buld/ARM/arch/generic/memhelper.hh for more info [mengjia]
    load_fault = inst->initiateAcc();

    // if translation delay, deferMem [mengjia]
    // in the case it is not the correct time to send the load
    // also defer it
    if ( (inst->isTranslationDelayed() || inst->fenceDelay()
                || inst->specTLBMiss()) &&
        load_fault == NoFault)
        return load_fault;

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    //
    // if it is faulty, not execute it, send it to commit, and commit statge will deal with it
    // here is signling the ROB, the inst can commit [mengjia]
    if (load_fault != NoFault || !inst->readPredicate()) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (!inst->readPredicate())
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "Load [sn:%lli] not executed from %s\n",
                inst->seqNum,
                (load_fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) ||
            inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
    } else {
        assert(inst->effAddrValid());
        int load_idx = inst->lqIdx;
        incrLdIdx(load_idx);

        if (checkLoads)
            return checkViolations(load_idx, inst);
    }

    return load_fault;
}

template <class Impl>
Fault
LSQUnit<Impl>::executeStore(DynInstPtr &store_inst)
{
    using namespace TheISA;
    // Make sure that a store exists.
    assert(stores != 0);

    int store_idx = store_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing store PC %s [sn:%lli]\n",
            store_inst->pcState(), store_inst->seqNum);

    assert(!store_inst->isSquashed());

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    int load_idx = store_inst->lqIdx;

    // TODO: Check whether this store tries to get an exclusive copy 
    // of target line [mengjia]
    Fault store_fault = store_inst->initiateAcc();

    if (store_inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!store_inst->readPredicate()) {
        DPRINTF(LSQUnit, "Store [sn:%lli] not executed from predication\n",
                store_inst->seqNum);
        store_inst->forwardOldRegs();
        return store_fault;
    }

    if (storeQueue[store_idx].size == 0) {
        DPRINTF(LSQUnit,"Fault on Store PC %s, [sn:%lli], Size = 0\n",
                store_inst->pcState(), store_inst->seqNum);

        return store_fault;
    }

    assert(store_fault == NoFault);

    if (store_inst->isStoreConditional()) {
        // Store conditionals need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB = true;

        ++storesToWB;
    }

    return checkViolations(load_idx, store_inst);

}

template <class Impl>
void
LSQUnit<Impl>::commitLoad()
{
    assert(loadQueue[loadHead]);

    if(loadQueue[loadHead]->isLoadIssued()){
      if(loadQueue[loadHead]->specWindowCycles == (Cycles) -1){
        removeSpecWindowQ(loadQueue[loadHead]);
      }
      loadQueue[loadHead]->calcSpecWindowCycles(specWindowLoadsHist,spectreWindowLoadsHist,cpu->curCycle());

      //Update Stats for Loads That Hit on Spec Reordered Loads.
      ++lsqCommitLoads;
      if(loadQueue[loadHead]->isHitOnLaterSpecLoad){
        ++lsqCommitSpecHitLoads;
        lsqSpecHitLoadsGainedCycles += (loadQueue[loadHead]->diffMemAccessTime);
      }         
    }
    
    
    
    DPRINTF(LSQUnit, "Committing head load instruction, PC %s\n",
            loadQueue[loadHead]->pcState());

    loadQueue[loadHead] = NULL;

    incrLdIdx(loadHead);

    --loads;
}

template <class Impl>
void
LSQUnit<Impl>::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loads == 0 || loadQueue[loadHead]);

    while (loads != 0 && loadQueue[loadHead]->seqNum <= youngest_inst) {
        commitLoad();
    }
}

template <class Impl>
void
LSQUnit<Impl>::commitStores(InstSeqNum &youngest_inst)
{
    assert(stores == 0 || storeQueue[storeHead].inst);

    int store_idx = storeHead;

    while (store_idx != storeTail) {
        assert(storeQueue[store_idx].inst);
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!storeQueue[store_idx].canWB) {
            if (storeQueue[store_idx].inst->seqNum > youngest_inst) {
                break;
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    storeQueue[store_idx].inst->pcState(),
                    storeQueue[store_idx].inst->seqNum);

            storeQueue[store_idx].canWB = true;

            ++storesToWB;
        }

        incrStIdx(store_idx);
    }
}

template <class Impl>
void
LSQUnit<Impl>::writebackPendingStore()
{
    if (hasPendingPkt) {
        assert(pendingPkt != NULL);

        if(pendingPkt->isWrite()){
            // If the cache is blocked, this will store the packet for retry.
            if (sendStore(pendingPkt)) {
                storePostSend(pendingPkt);
            }
            pendingPkt = NULL;
            hasPendingPkt = false;
        }
    }
}




// [InvisiSpec] update FenceDelay State
template <class Impl>
void
LSQUnit<Impl>::updateVisibleState()
{
    int load_idx = loadHead;

    //iterate all the loads and update its fencedelay state accordingly
    while (load_idx != loadTail && loadQueue[load_idx]){
        DynInstPtr inst = loadQueue[load_idx];

        if (!loadInExec){

            if ( (isFuturistic && inst->isPrevInstsCommitted()) ||
                    (!isFuturistic && inst->isPrevBrsCommitted())){
                if (inst->fenceDelay()){
                    DPRINTF(LSQUnit, "Clear virtual fence for "
                            "inst [sn:%lli] PC %s\n",
                        inst->seqNum, inst->pcState());
                }
                inst->fenceDelay(false);
            }else {
                if (!inst->fenceDelay()){
                    DPRINTF(LSQUnit, "Deffering an inst [sn:%lli] PC %s"
                            " due to virtual fence\n",
                        inst->seqNum, inst->pcState());
                }
                inst->fenceDelay(true);
            }
            inst->readyToExpose(true);
        } else if (loadInExec && isInvisibleSpec){

            if ( (isFuturistic && inst->isPrevInstsCompleted()) ||
                    (!isFuturistic && inst->isPrevBrsResolved())){
                if (!inst->readyToExpose()){
                    DPRINTF(LSQUnit, "Set readyToExpose for "
                            "inst [sn:%lli] PC %s\n",
                        inst->seqNum, inst->pcState());
                    if (inst->needPostFetch()){
                        ++loadsToVLD;
                    }
                }
                inst->readyToExpose(true);
            }else {
                if (inst->readyToExpose()){
                    DPRINTF(LSQUnit, "The load can not be validated "
                            "[sn:%lli] PC %s\n",
                        inst->seqNum, inst->pcState());
                    assert(0);
                    //--loadsToVLD;
                }
                inst->readyToExpose(false);
            }
            inst->fenceDelay(false);
        } else {
            inst->readyToExpose(true);
            inst->fenceDelay(false);
        }
        incrLdIdx(load_idx);
    }
}

// [InvisiSpec] validate loads
template <class Impl>
int
LSQUnit<Impl>::exposeLoads()
{
    if(!isInvisibleSpec){
        assert(loadsToVLD==0
            && "request validation on Non invisible Spec mode");
    }

    int old_loadsToVLD = loadsToVLD;

    // [InvisiSpec] Note:
    // need to iterate from the head every time
    // since the load can be exposed out-of-order
    int loadVLDIdx = loadHead;

    while (loadsToVLD > 0 &&
        loadVLDIdx != loadTail &&
        loadQueue[loadVLDIdx]) {

        if (loadQueue[loadVLDIdx]->isSquashed()){
            incrLdIdx(loadVLDIdx);
            continue;
        }
        // skip the loads that either do not need to expose
        // or exposed already
        if(!loadQueue[loadVLDIdx]->needPostFetch()
                || loadQueue[loadVLDIdx]->isExposeSent() ){
            incrLdIdx(loadVLDIdx);
            continue;
        }

        DynInstPtr load_inst = loadQueue[loadVLDIdx];
        if (loadQueue[loadVLDIdx]->fault!=NoFault){
            //load is executed, so it wait for expose complete
            //to send it to commit, regardless of whether it is ready
            //to expose
            load_inst->setExposeCompleted();
            load_inst->setExposeSent();
            loadsToVLD--;
            if (load_inst->isExecuted()){
                DPRINTF(LSQUnit, "Execute finished and gets violation fault."
                    "Send inst [sn:%lli] to commit stage.\n",
                    load_inst->seqNum);
                    iewStage->instToCommit(load_inst);
                    iewStage->activityThisCycle();
            }
            incrLdIdx(loadVLDIdx);
            continue;
        }

        // skip the loads that need expose but
        // are not ready
        if (loadQueue[loadVLDIdx]->needPostFetch()
                && !loadQueue[loadVLDIdx]->readyToExpose()){
            incrLdIdx(loadVLDIdx);
            continue;
        }

        assert(loadQueue[loadVLDIdx]->needPostFetch()
                && loadQueue[loadVLDIdx]->readyToExpose() );

        assert(!load_inst->isCommitted());


        Request *req = load_inst->postReq;
        Request *sreqLow = load_inst->postSreqLow;
        Request *sreqHigh = load_inst->postSreqHigh;

        // we should not have both req and sreqLow not NULL
        assert( !(req && sreqLow));

        DPRINTF(LSQUnit, "Validate/Expose request for inst [sn:%lli]"
            " PC= %s. req=%#x, reqLow=%#x, reqHigh=%#x\n",
            load_inst->seqNum, load_inst->pcState(),
            (Addr)(load_inst->postReq),
            (Addr)(load_inst->postSreqLow), (Addr)(load_inst->postSreqHigh));

        PacketPtr data_pkt = NULL;
        PacketPtr snd_data_pkt = NULL;

        LSQSenderState *state = new LSQSenderState;
        state->isLoad = false;
        state->idx = loadVLDIdx;
        state->inst = load_inst;
        state->noWB = true;

        bool split = false;
        if (TheISA::HasUnalignedMemAcc && sreqLow) {
            split = true;
        } else {
            assert(req);
        }

        bool onlyExpose = false;
        if (!split) {
            if (load_inst->needExposeOnly() || load_inst->isL1HitLow()){
                data_pkt = Packet::createExpose(req);
                onlyExpose = true;
            }else {
                data_pkt = Packet::createValidate(req);
                if (!load_inst->vldData)
                    load_inst->vldData = new uint8_t[1];
                data_pkt->dataStatic(load_inst->vldData);
            }
            data_pkt->senderState = state;
            data_pkt->setFirst();
            data_pkt->reqIdx = loadVLDIdx;
            DPRINTF(LSQUnit, "contextid = %d\n", req->contextId());
        } else {
            // allocate memory if we need at least one validation
            if (!load_inst->needExposeOnly() &&
                (!load_inst->isL1HitLow() || !load_inst->isL1HitHigh())){
                if (!load_inst->vldData)
                    load_inst->vldData = new uint8_t[2];
            } else {
                onlyExpose = true;
            }

            // Create the split packets. - first one
            if (load_inst->needExposeOnly() || load_inst->isL1HitLow()){
                data_pkt = Packet::createExpose(sreqLow);
            }else{
                data_pkt = Packet::createValidate(sreqLow);
                assert(load_inst->vldData);
                data_pkt->dataStatic(load_inst->vldData);
            }

            // Create the split packets. - second one
            if (load_inst->needExposeOnly() || load_inst->isL1HitHigh()){
                snd_data_pkt = Packet::createExpose(sreqHigh);
            } else {
                snd_data_pkt = Packet::createValidate(sreqHigh);
                assert(load_inst->vldData);
                snd_data_pkt->dataStatic(&(load_inst->vldData[1]));
            }

            data_pkt->senderState = state;
            data_pkt->setFirst();
            snd_data_pkt->senderState = state;
            data_pkt->reqIdx = loadVLDIdx;
            snd_data_pkt->reqIdx = loadVLDIdx;

            data_pkt->isSplit = true;
            snd_data_pkt->isSplit = true;
            state->isSplit = true;
            state->outstanding = 2;
            state->mainPkt = data_pkt;

            DPRINTF(LSQUnit, "contextid = %d, %d\n",
                    sreqLow->contextId(), sreqHigh->contextId());
            req = sreqLow;
        }

        assert(!req->isStrictlyOrdered());
        assert(!req->isMmappedIpr());

        DPRINTF(LSQUnit, "D-Cache: Validating/Exposing load idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                loadVLDIdx, load_inst->pcState(),
                //FIXME: resultData not memData
                req->getPaddr(), (int)*(load_inst->memData),
                load_inst->seqNum);

        bool successful_expose = true;
        bool completedFirst = false;

        if (!dcachePort->sendTimingReq(data_pkt)){
            DPRINTF(IEW, "D-Cache became blocked when "
                "validating [sn:%lli], will retry later\n",
                load_inst->seqNum);
            successful_expose = false;
        } else {
            if (split) {
                // If split, try to send the second packet too
                completedFirst = true;
                assert(snd_data_pkt);

                if (!dcachePort->sendTimingReq(snd_data_pkt)){
                    state->complete();
                    state->cacheBlocked = true;
                    successful_expose = false;
                    DPRINTF(IEW, "D-Cache became blocked when validating"
                        " [sn:%lli] second packet, will retry later\n",
                        load_inst->seqNum);
                }
            }
        }

        if (!successful_expose){
            if (!split) {
                delete state;
                delete data_pkt;
            }else{
                if (!completedFirst){
                    delete state;
                    delete data_pkt;
                    delete snd_data_pkt;
                } else {
                    delete snd_data_pkt;
                }
            }
            //cpu->wakeCPU(); // This will cause issue(wrong activity count and affects the memory transactions
            ++lsqCacheBlocked;
            break;
        } else {
            // Here is to fix memory leakage
            // it is ugly, but we have to do it now.
            load_inst->needDeletePostReq(false);

            // if all the packets we sent out is expose,
            // we assume the expose is alreay completed
            if (onlyExpose) {
                load_inst->setExposeCompleted();
                numExposes++;
            } else {
                numValidates++;
            }
            if (load_inst->needExposeOnly()){
                numConvertedExposes++;
            }
            if (load_inst->isExecuted() && load_inst->isExposeCompleted()
                    && !load_inst->isSquashed()){
                DPRINTF(LSQUnit, "Expose finished. Execution done."
                    "Send inst [sn:%lli] to commit stage.\n",
                    load_inst->seqNum);
                    iewStage->instToCommit(load_inst);
                    iewStage->activityThisCycle();
            } else{
                DPRINTF(LSQUnit, "Need validation or execution not finishes."
                    "Need to wait for readResp/validateResp "
                    "for inst [sn:%lli].\n",
                    load_inst->seqNum);
            }

            load_inst->setExposeSent();
            --loadsToVLD;
            incrLdIdx(loadVLDIdx);
            if (!split){
                setSpecBuffState(req);
            } else {
                setSpecBuffState(sreqLow);
                setSpecBuffState(sreqHigh);
            }
        }
    }

    /* DPRINTF(LSQUnit, "Send validate/expose for %d insts. loadsToVLD=%d" */
    /*         ". loadHead=%d. loadTail=%d.\n", */
    /*         old_loadsToVLD-loadsToVLD, loadsToVLD, loadHead, */
    /*         loadTail); */

    assert(loads>=0 && loadsToVLD >= 0);

    return old_loadsToVLD-loadsToVLD;
}




template <class Impl>
void
LSQUnit<Impl>::writebackStores()
{
    // First writeback the second packet from any split store that didn't
    // complete last cycle because there weren't enough cache ports available.
    if (TheISA::HasUnalignedMemAcc) {
        writebackPendingStore();
    }

    while (storesToWB > 0 &&
           storeWBIdx != storeTail &&
           storeQueue[storeWBIdx].inst &&
           storeQueue[storeWBIdx].canWB &&
           ((!needsTSO) || (!storeInFlight)) &&
           usedStorePorts < cacheStorePorts) {

        if (isStoreBlocked) {
            DPRINTF(LSQUnit, "Unable to write back any more stores, cache"
                    " is blocked on stores!\n");
            break;
        }

        // Store didn't write any data so no need to write it back to
        // memory.
        if (storeQueue[storeWBIdx].size == 0) {
            completeStore(storeWBIdx);

            incrStIdx(storeWBIdx);

            continue;
        }

        ++usedStorePorts;

        if (storeQueue[storeWBIdx].inst->isDataPrefetch()) {
            incrStIdx(storeWBIdx);

            continue;
        }

        assert(storeQueue[storeWBIdx].req);
        assert(!storeQueue[storeWBIdx].committed);

        if (TheISA::HasUnalignedMemAcc && storeQueue[storeWBIdx].isSplit) {
            assert(storeQueue[storeWBIdx].sreqLow);
            assert(storeQueue[storeWBIdx].sreqHigh);
        }

        DynInstPtr inst = storeQueue[storeWBIdx].inst;

        Request *req = storeQueue[storeWBIdx].req;
        RequestPtr sreqLow = storeQueue[storeWBIdx].sreqLow;
        RequestPtr sreqHigh = storeQueue[storeWBIdx].sreqHigh;

        storeQueue[storeWBIdx].committed = true;

        assert(!inst->memData);
        inst->memData = new uint8_t[req->getSize()];

        if (storeQueue[storeWBIdx].isAllZeros)
            memset(inst->memData, 0, req->getSize());
        else
            memcpy(inst->memData, storeQueue[storeWBIdx].data, req->getSize());

        PacketPtr data_pkt;
        PacketPtr snd_data_pkt = NULL;

        LSQSenderState *state = new LSQSenderState;
        state->isLoad = false;
        state->idx = storeWBIdx;
        state->inst = inst;

        if (!TheISA::HasUnalignedMemAcc || !storeQueue[storeWBIdx].isSplit) {

            // Build a single data packet if the store isn't split.
            data_pkt = Packet::createWrite(req);
            data_pkt->dataStatic(inst->memData);
            data_pkt->senderState = state;
        } else {
            // Create two packets if the store is split in two.
            data_pkt = Packet::createWrite(sreqLow);
            snd_data_pkt = Packet::createWrite(sreqHigh);

            data_pkt->dataStatic(inst->memData);
            snd_data_pkt->dataStatic(inst->memData + sreqLow->getSize());

            data_pkt->senderState = state;
            snd_data_pkt->senderState = state;

            state->isSplit = true;
            state->outstanding = 2;

            // Can delete the main request now.
            delete req;
            req = sreqLow;
        }

        DPRINTF(LSQUnit, "D-Cache: Writing back store idx:%i PC:%s "
                "to Addr:%#x, data:%#x [sn:%lli]\n",
                storeWBIdx, inst->pcState(),
                req->getPaddr(), (int)*(inst->memData),
                inst->seqNum);

        // @todo: Remove this SC hack once the memory system handles it.
        if (inst->isStoreConditional()) {
            assert(!storeQueue[storeWBIdx].isSplit);
            // Disable recording the result temporarily.  Writing to
            // misc regs normally updates the result, but this is not
            // the desired behavior when handling store conditionals.
            inst->recordResult(false);
            bool success = TheISA::handleLockedWrite(inst.get(), req, cacheBlockMask);
            inst->recordResult(true);

            if (!success) {
                // Instantly complete this store.
                DPRINTF(LSQUnit, "Store conditional [sn:%lli] failed.  "
                        "Instantly completing it.\n",
                        inst->seqNum);
                WritebackEvent *wb = new WritebackEvent(inst, data_pkt, this);
                cpu->schedule(wb, curTick() + 1);
                completeStore(storeWBIdx);
                incrStIdx(storeWBIdx);
                continue;
            }
        } else {
            // Non-store conditionals do not need a writeback.
            state->noWB = true;
        }

        bool split =
            TheISA::HasUnalignedMemAcc && storeQueue[storeWBIdx].isSplit;

        ThreadContext *thread = cpu->tcBase(lsqID);

        if (req->isMmappedIpr()) {
            assert(!inst->isStoreConditional());
            TheISA::handleIprWrite(thread, data_pkt);
            delete data_pkt;
            if (split) {
                assert(snd_data_pkt->req->isMmappedIpr());
                TheISA::handleIprWrite(thread, snd_data_pkt);
                delete snd_data_pkt;
                delete sreqLow;
                delete sreqHigh;
            }
            delete state;
            delete req;
            completeStore(storeWBIdx);
            incrStIdx(storeWBIdx);
        } else if (!sendStore(data_pkt)) {
            DPRINTF(IEW, "D-Cache became blocked when writing [sn:%lli], will"
                    "retry later\n",
                    inst->seqNum);

            // Need to store the second packet, if split.
            if (split) {
                state->pktToSend = true;
                state->pendingPacket = snd_data_pkt;
            }
        } else {

            // If split, try to send the second packet too
            if (split) {
                assert(snd_data_pkt);

                // Ensure there are enough ports to use.
                if (usedStorePorts < cacheStorePorts) {
                    ++usedStorePorts;
                    if (sendStore(snd_data_pkt)) {
                        storePostSend(snd_data_pkt);
                    } else {
                        DPRINTF(IEW, "D-Cache became blocked when writing"
                                " [sn:%lli] second packet, will retry later\n",
                                inst->seqNum);
                    }
                } else {

                    // Store the packet for when there's free ports.
                    assert(pendingPkt == NULL);
                    pendingPkt = snd_data_pkt;
                    hasPendingPkt = true;
                }
            } else {

                // Not a split store.
                storePostSend(data_pkt);
            }
        }
    }

    // Not sure this should set it to 0.
    usedStorePorts = 0;

    assert(stores >= 0 && storesToWB >= 0);
}

/*template <class Impl>
void
LSQUnit<Impl>::removeMSHR(InstSeqNum seqNum)
{
    list<InstSeqNum>::iterator mshr_it = find(mshrSeqNums.begin(),
                                              mshrSeqNums.end(),
                                              seqNum);

    if (mshr_it != mshrSeqNums.end()) {
        mshrSeqNums.erase(mshr_it);
        DPRINTF(LSQUnit, "Removing MSHR. count = %i\n",mshrSeqNums.size());
    }
}*/


/** [CleanupCache] Counts loads that need to be squashed. */

template <class Impl>
int
LSQUnit<Impl>::num_loads_to_squash(const InstSeqNum &squashed_num)
{
  DPRINTF(LSQUnit,"Will Squash_Loads (Addr,Seq):\n");

  int count = 0;
  
  int load_idx = loadTail;
  decrLdIdx(load_idx);
  
  while ((loads-count) != 0 && loadQueue[load_idx]->seqNum > squashed_num) {
    DPRINTF(LSQUnit,"(0x%llx,%ld),\n",loadQueue[load_idx]->physEffAddrLow & cacheBlockMask,loadQueue[load_idx]->seqNum );          
    decrLdIdx(load_idx);
    ++count;
  }

  
  DPRINTF(LSQUnit,"Total: %d\n",count);
  return count; 
}

/** [CleanupCache] Counts loads that need to be squashed. */

template <class Impl>
int
LSQUnit<Impl>::num_lines_to_invalidate(const InstSeqNum &squashed_num)
{

  DPRINTF(LSQUnit,"Will Inv_Lines (Addr,Seq):\n");
  
  int count = 0;
  int total_count = 0;
  
  int load_idx = loadTail;
  decrLdIdx(load_idx);
  
  while ((loads-total_count) != 0 && loadQueue[load_idx]->seqNum > squashed_num) {

    //L1 cache miss
    if(!loadQueue[load_idx]->isL1HitCC()){
      ++count;
      DPRINTF(LSQUnit,"L1-MISS: (%#x,sn:%ld -> %d,%d,%d,%d),\n",loadQueue[load_idx]->physEffAddrLow,loadQueue[load_idx]->seqNum,loadQueue[load_idx]->isL1HitCC(),loadQueue[load_idx]->isL2HitCC(),loadQueue[load_idx]->isL2MissCC(),loadQueue[load_idx]->isLoadReturned());  
    } else {
      DPRINTF(LSQUnit,"L1-HIT:  (%#x,%ld -> %d,%d,%d,%d),\n",loadQueue[load_idx]->physEffAddrLow,loadQueue[load_idx]->seqNum,loadQueue[load_idx]->isL1HitCC(),loadQueue[load_idx]->isL2HitCC(),loadQueue[load_idx]->isL2MissCC(),loadQueue[load_idx]->isLoadReturned());    
    }
    total_count++;    
    decrLdIdx(load_idx);
    
  }

  DPRINTF(LSQUnit,"Total: %d\n",count);
  
  return count; 
}

template <class Impl>
void
LSQUnit<Impl>::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loads, stores);

    // [CleanupCache] Count number of squash events
    ++lsqSquashEvents;

    int load_idx = loadTail;
    int loads_to_squash = loads;
    decrLdIdx(load_idx);

    if(!squash_in_progress){
      assert(loadsInflightCleanupIssued ==0);
      assert(loadsExecutedCleanupIssued ==0);
      assert(cleanupAcksReceived == 0);    
      assert(skippedCleanupReqs == 0);

      lsqCleanupSquashStartCycle = cpu->curCycle();
      cleanup_l1hits_exec_start = lsqSquashedLoadsExec_L1Hits.value();
      cleanup_l2hits_exec_start = lsqSquashedLoadsExec_L2Hits.value();
      cleanup_l2miss_exec_start = lsqSquashedLoadsExec_L2Miss.value();
      cleanup_l1hits_inflight_start = lsqSquashedLoadsInflight_L1Hits.value();
      cleanup_l2hits_inflight_start = lsqSquashedLoadsInflight_L2Hits.value();
      cleanup_l2miss_inflight_start = lsqSquashedLoadsInflight_L2Miss.value();
      cleanup_total_start = lsqSquashedLoadsInflight_Tot.value() + lsqSquashedLoadsExec_Tot.value() ;
      
    }

    DPRINTF(LSQUnit, "Squashing till [sn:%lli]: Currently squash_in_progress:%d, and squashfrom_seqnum:[sn:%lli]\n", squashed_num,squash_in_progress,squashfrom_seqnum);

    if(cleanupLQExecd_on_Squash || cleanupLQInflight_on_Squash || cleanupL2Inv){
      if(squash_in_progress){
        assert(squashed_num <= squashfrom_seqnum);
      } else if(loads != 0){ //只要没有需要clean的load，就不使用cleanupspec
        squash_in_progress = true;
      }
    
      DPRINTF(LSQUnit, "For squash till [sn:%lli] SkippedCleanupReqs = %d and squash_in_progress:%d\n",squashed_num, skippedCleanupReqs,squash_in_progress);
      //dumpInsts();

      
      //Iterate through LSQ and squash, While there are loads to be squashed & not touched before.
      while ((loads_to_squash != 0) && (loadQueue[load_idx]->seqNum > squashed_num) ) {

      
        if(!loadQueue[load_idx]->isSquashTouched()){

          if(loadQueue[load_idx]->isLoadIssued()) {
            loadQueue[load_idx]->calcSpecWindowCycles(specWindowLoadsHist,spectreWindowLoadsHist,cpu->curCycle());
            removeSpecWindowQ(loadQueue[load_idx]);
          }

          DPRINTF(LSQUnit,"Squashing & Buffering Unsafe Load Instruction PC %s, "
                  "[sn:%lli], Addr:%#x, LoadIssued:%d, LoadReturned:%d\n",
                  loadQueue[load_idx]->pcState(),
                  loadQueue[load_idx]->seqNum,
                  loadQueue[load_idx]->physEffAddrLow,
                  loadQueue[load_idx]->isLoadIssued(),
                  loadQueue[load_idx]->isLoadReturned());

          if (isStalled() && load_idx == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
          }

          if (loadQueue[load_idx]->needPostFetch() &&
              loadQueue[load_idx]->readyToExpose() &&
              !loadQueue[load_idx]->isExposeSent()){
            loadsToVLD --;
          }


          //TODO: Check if setSquashed causes commit to do something weird
          loadQueue[load_idx]->setSquashed();
          loadQueue[load_idx]->isSquashTouched(true); //Touched by a Squash

          //Push into a buffer
          loadQueue[load_idx]->setSquashBuffered(true); //Pushed into a virtual queue for squashed loads.

        }      
        --loads_to_squash;
        decrLdIdx(load_idx);
      }

      //Update squashed from where?
      squashfrom_seqnum = squashed_num;
    
      //Check if All Reqs Sent from Buffered Queue
      squashbuffer_used = false;
      squashbuffer_empty = false;
      squashbuffer_emptying_started = false; 

      if(squash_in_progress && issueBufferedCleanupReqs()){
        DPRINTF(LSQUnit,"Buffered CleanupReqs All Sent to Cache For Blocking Squash on [sn:%lli]\n",squashed_num);
      } else {
        DPRINTF(LSQUnit,"Buffered CleanupReqs Waiting for ROB-Head to be required inst for Squash on [sn:%lli]\n",squashed_num);
      }
    
      //Wait until all Acks Received & Clear LoadQ
    
      if(squash_in_progress && clearLoadQueue()){
        DPRINTF(LSQUnit,"LoadQ Finally Cleared For Blocking Squash on [sn:%lli]\n",squashed_num);
      } else{
        DPRINTF(LSQUnit,"LoadQ Will Get Cleared When Blocking CleanupAcks Received for Squash on [sn:%lli]\n",squashed_num);
      }         
    } else {

      //Regular Squash for Baseline:
      while (loads != 0 && (loadQueue[load_idx]->seqNum > squashed_num)) {
        
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue[load_idx]->pcState(),
                loadQueue[load_idx]->seqNum);

        // Remove entry from Spec-Window Tracking Queue.
        if(loadQueue[load_idx]->isLoadIssued()) {
          loadQueue[load_idx]->calcSpecWindowCycles(specWindowLoadsHist,spectreWindowLoadsHist,cpu->curCycle());
          removeSpecWindowQ(loadQueue[load_idx]);
        }


        if (isStalled() && load_idx == stallingLoadIdx) {
          stalled = false;
          stallingStoreIsn = 0;
          stallingLoadIdx = 0;
        }
        
        if (loadQueue[load_idx]->needPostFetch() &&
            loadQueue[load_idx]->readyToExpose() &&
            !loadQueue[load_idx]->isExposeSent()){
          loadsToVLD --;
        }
        
        // Clear the smart pointer to make sure it is decremented.    
        loadQueue[load_idx]->setSquashed();
        loadQueue[load_idx] = NULL;
        --loads;

        loadTail = load_idx;
        decrLdIdx(load_idx);
        ++lsqSquashedLoadsTot;
      }      
    }
    
    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
      memDepViolator = NULL;
    }

    int store_idx = storeTail;
    decrStIdx(store_idx);

    while (stores != 0 &&
           storeQueue[store_idx].inst->seqNum > squashed_num) {
      // Instructions marked as can WB are already committed.
      if (storeQueue[store_idx].canWB) {
        break;
      }

      DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
              "idx:%i [sn:%lli]\n",
              storeQueue[store_idx].inst->pcState(),
              store_idx, storeQueue[store_idx].inst->seqNum);

      // I don't think this can happen.  It should have been cleared
      // by the stalling load.
      if (isStalled() &&
          storeQueue[store_idx].inst->seqNum == stallingStoreIsn) {
        panic("Is stalled should have been cleared by stalling load!\n");
        stalled = false;
        stallingStoreIsn = 0;
      }

      // Clear the smart pointer to make sure it is decremented.
      storeQueue[store_idx].inst->setSquashed();
      storeQueue[store_idx].inst = NULL;
      storeQueue[store_idx].canWB = 0;

      // Must delete request now that it wasn't handed off to
      // memory.  This is quite ugly.  @todo: Figure out the proper
      // place to really handle request deletes.
      delete storeQueue[store_idx].req;
      if (TheISA::HasUnalignedMemAcc && storeQueue[store_idx].isSplit) {
        delete storeQueue[store_idx].sreqLow;
        delete storeQueue[store_idx].sreqHigh;

        storeQueue[store_idx].sreqLow = NULL;
        storeQueue[store_idx].sreqHigh = NULL;
      }

      storeQueue[store_idx].req = NULL;
      --stores;

      // Inefficient!
      storeTail = store_idx;

      decrStIdx(store_idx);
      ++lsqSquashedStores;
    }
}


// after sent, we assume the store is complete
// thus, we can wekeup and forward data
// In TSO, mark inFlightStore as true to block following stores [mengjia]
template <class Impl>
void
LSQUnit<Impl>::storePostSend(PacketPtr pkt)
{
    if (isStalled() &&
        storeQueue[storeWBIdx].inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx]);
    }

    if (!storeQueue[storeWBIdx].inst->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeQueue[storeWBIdx].inst->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeQueue[storeWBIdx].inst);
        }
    }

    if (needsTSO) {
        storeInFlight = true;
    }

    DPRINTF(LSQUnit, "Post sending store for inst [sn:%lli]\n",
            storeQueue[storeWBIdx].inst->seqNum);
    incrStIdx(storeWBIdx);
}



template <class Impl>
void
LSQUnit<Impl>::completeValidate(DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();
    // if instruction fault, no need to check value,
    // return directly
    //assert(!inst->needExposeOnly());
    if (inst->isExposeCompleted() || inst->isSquashed()){
        //assert(inst->fault != NoFault);
        //Already sent to commit, do nothing
        return;
    }
    //Check validation result
    bool validation_fail = false;
    if (!inst->isL1HitLow() && inst->vldData[0]==0) {
        validation_fail = true;
    } else {
        if (pkt->isSplit && !inst->isL1HitHigh()
            && inst->vldData[1]==0){
            validation_fail = true;
        }
    }
    if (validation_fail){
        // Mark the load for re-execution
        inst->fault = std::make_shared<ReExec>();
        inst->validationFail(true);
        DPRINTF(LSQUnit, "Validation failed.\n",
        inst->seqNum);
    }

    inst->setExposeCompleted();
    if ( inst->isExecuted() && inst->isExposeCompleted() ){
      DPRINTF(LSQUnit, //"Validation finished. Execution done."
            "Send inst [sn:%lli] to commit stage.\n",
            inst->seqNum);
            iewStage->instToCommit(inst);
            iewStage->activityThisCycle();
    } else{
      DPRINTF(LSQUnit, //"Validation done. Execution not finishes."
            "Need to wait for readResp for inst [sn:%lli].\n",
            inst->seqNum);
    }
}

template <class Impl>
void
LSQUnit<Impl>::writeback(DynInstPtr &inst, PacketPtr pkt)
{
    iewStage->wakeCPU();
    iewStage->activityThisCycle();

    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert(!inst->isStore());
        ++lsqIgnoredResponses;
        return;
    }

    //DPRINTF(LSQUnit, "write back for inst [sn:%lli]\n", inst->seqNum);
    assert(!(inst->isExecuted() && inst->isExposeCompleted() &&
                inst->fault==NoFault) &&
            "in this case, we will put it into ROB twice.");

    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec.
            assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr);

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }

    // [mengjia]
    // check schemes to decide whether to set load can be committed
    // on receiving readResp or readSpecResp
    if(!isInvisibleSpec){
        // if not invisibleSpec mode, we only receive readResp
        assert(!pkt->isSpec() && !pkt->isValidate() &&
                "Receiving spec or validation response "
                "in non invisibleSpec mode");
        iewStage->instToCommit(inst);
    } else if (inst->fault != NoFault){
        inst->setExposeCompleted();
        inst->setExposeSent();
        iewStage->instToCommit(inst);
    } else {
        //isInvisibleSpec == true
        if (pkt->isSpec()) {
            inst->setSpecCompleted();
        }

        assert(!pkt->isValidate() && "receiving validation response"
                "in invisibleSpec RC mode");
        assert(!pkt->isExpose() && "receiving expose response"
                "on write back path");

        // check whether the instruction can be committed
        if ( !inst->isExposeCompleted() && inst->needPostFetch() ){
            DPRINTF(LSQUnit, "Expose not finished. "
                "Wait until expose completion"
                " to send inst [sn:%lli] to commit stage\n", inst->seqNum);
        }else{
            DPRINTF(LSQUnit, "Expose and execution both finished. "
                "Send inst [sn:%lli] to commit stage\n", inst->seqNum);
            iewStage->instToCommit(inst);
        }

    }

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

// set store to complete [mengjia]
// complete the store after it commits
template <class Impl>
void
LSQUnit<Impl>::completeStore(int store_idx)
{
    assert(storeQueue[store_idx].inst);
    storeQueue[store_idx].completed = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    if (store_idx == storeHead) {
        do {
            incrStIdx(storeHead);

            --stores;
        } while (storeQueue[storeHead].completed &&
                 storeHead != storeTail);

        iewStage->updateLSQNextCycle = true;
    }

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            storeQueue[store_idx].inst->seqNum, store_idx, storeHead);

#if TRACING_ON
    if (DTRACE(O3PipeView)) {
        storeQueue[store_idx].inst->storeTick =
            curTick() - storeQueue[store_idx].inst->fetchTick;
    }
#endif

    if (isStalled() &&
        storeQueue[store_idx].inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%i\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx]);
    }

    storeQueue[store_idx].inst->setCompleted();

    if (needsTSO) {
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.

    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker && !storeQueue[store_idx].inst->isStoreConditional()) {
        cpu->checker->verify(storeQueue[store_idx].inst);
    }
}

template <class Impl>
bool
LSQUnit<Impl>::sendStore(PacketPtr data_pkt)
{
    if (!dcachePort->sendTimingReq(data_pkt)) {
        // Need to handle becoming blocked on a store.
        isStoreBlocked = true;
        ++lsqCacheBlocked;
        assert(retryPkt == NULL);
        retryPkt = data_pkt;
        return false;
    }
    setSpecBuffState(data_pkt->req);
    return true;
}



template <class Impl>
void
LSQUnit<Impl>::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: store blocked\n");
        assert(retryPkt != NULL);
        assert(retryPkt->isWrite());

        LSQSenderState *state =
            dynamic_cast<LSQSenderState *>(retryPkt->senderState);

        if (dcachePort->sendTimingReq(retryPkt)) {
            // Don't finish the store unless this is the last packet.
            if (!TheISA::HasUnalignedMemAcc || !state->pktToSend ||
                    state->pendingPacket == retryPkt) {
                state->pktToSend = false;
                storePostSend(retryPkt);
            }
            retryPkt = NULL;
            isStoreBlocked = false;

            // Send any outstanding packet.
            if (TheISA::HasUnalignedMemAcc && state->pktToSend) {
                assert(state->pendingPacket);
                if (sendStore(state->pendingPacket)) {
                    storePostSend(state->pendingPacket);
                }
            }
        } else {
            // Still blocked!
            ++lsqCacheBlocked;
        }
    }
}

template <class Impl>
inline void
LSQUnit<Impl>::incrStIdx(int &store_idx) const
{
    if (++store_idx >= SQEntries)
        store_idx = 0;
}

template <class Impl>
inline void
LSQUnit<Impl>::decrStIdx(int &store_idx) const
{
    if (--store_idx < 0)
        store_idx += SQEntries;
}

template <class Impl>
inline void
LSQUnit<Impl>::incrLdIdx(int &load_idx) const
{
    if ((++load_idx) >= LQEntries)
        load_idx = 0;
}

template <class Impl>
inline void
LSQUnit<Impl>::decrLdIdx(int &load_idx) const
{
    if ((--load_idx) < 0)
        load_idx += LQEntries;
}

template <class Impl>
void
LSQUnit<Impl>::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loads);
    cprintf("Load queue: ");

    int load_idx = loadHead;

    while (load_idx != loadTail && loadQueue[load_idx]) {
        const DynInstPtr &inst(loadQueue[load_idx]);
        cprintf("%s.[sn:%i]-%#x_%d_%d ", inst->pcState(), inst->seqNum,printAddress(loadQueue[load_idx]->physEffAddrLow),loadQueue[load_idx]->isLoadIssued(),loadQueue[load_idx]->isLoadReturned());
        incrLdIdx(load_idx);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", stores);
    cprintf("Store queue: ");

    int store_idx = storeHead;

    while (store_idx != storeTail && storeQueue[store_idx].inst) {
        const DynInstPtr &inst(storeQueue[store_idx].inst);
        cprintf("%s.[sn:%i] ", inst->pcState(), inst->seqNum);

        incrStIdx(store_idx);
    }

    cprintf("\n");
}


// [CleanupCache] Pop buffered cleanup requests for loads being squashed, when squashedSeq is top of ROB.
template <class Impl>
bool
LSQUnit<Impl>::issueBufferedCleanupReqs()
{
  assert(squash_in_progress == true);
  if(squashbuffer_used && squashbuffer_empty){
    DPRINTF(LSQUnit, "SquashBuffer Emptied : Waiting for CleanupAcks for Load-Squash till [sn:%lli]. NeedAcks:%d, GotAcks:%d, SkippedReqs:%d, squashbuffer_used:%d\n",
            squashfrom_seqnum, loadsInflightCleanupIssued + loadsExecutedCleanupIssued, cleanupAcksReceived,skippedCleanupReqs,squashbuffer_used);

    return true;
  }
    
  // Check if squashfrom_seq == (ROB-done +1), if yes then proceed. Else, return false;
  if( (iewStage->getDoneSeqNumCommit(lsqID) < (squashfrom_seqnum -1)) && (iewStage->getDoneSeqNumCommit(lsqID) > 0) ){ 
    DPRINTF(LSQUnit,"**Squash Buffer Waiting for Done Seq Num : %lli to reach one before Squash Head : %lli.\n", \
            iewStage->getDoneSeqNumCommit(lsqID), squashfrom_seqnum);
    return false;
  } else {
    DPRINTF(LSQUnit,"**Squash Buffer Done-Waiting for Done Seq Num : %lli to reach one before Squash Head : %lli. Clearing SquashBuffer\n", \
            iewStage->getDoneSeqNumCommit(lsqID), squashfrom_seqnum);
    
    if(!squashbuffer_emptying_started){
      lsqCleanupSquashBufferEmptyStartCycle = cpu->curCycle();
      squashbuffer_emptying_started = true;
    }
  }
  
  
  InstSeqNum issue_SquashedFromSeqNum = squashfrom_seqnum;
  
  int load_idx = loadTail;
  int loads_to_squash = loads;
  decrLdIdx(load_idx);

      
  while ((loads_to_squash != 0) && (loadQueue[load_idx]->seqNum > issue_SquashedFromSeqNum) ) {

    if(loadQueue[load_idx]->isSquashBuffered()){
       
      DPRINTF(LSQUnit,"Attempting to Issue Buffered CleanupReq for Load Instruction PC %s, "
              "[sn:%lli], Addr:%#x, LoadIssued:%d, LoadReturned:%d\n",
              loadQueue[load_idx]->pcState(),
              loadQueue[load_idx]->seqNum,
              loadQueue[load_idx]->physEffAddrLow,
              loadQueue[load_idx]->isLoadIssued(),
              loadQueue[load_idx]->isLoadReturned());

      //Give an opprotunity for skipped loads in previous iteration to be issued.
      if(loadQueue[load_idx]->isCleanupReqSkipped()){
        --skippedCleanupReqs;
        loadQueue[load_idx]->setCleanupReqSkipped(false);
      }
    
      //[CleanupCache: Update squash statistics & take appropriate action.
      if(loadQueue[load_idx]->isSplitReq()){
        ++lsqSquashedLoadsSplitReq;
        //TODO: Cleanup For SplitReq
        loadQueue[load_idx]->setSquashBuffered(false); //Cleanup Done 
        
      } else {
        if(!loadQueue[load_idx]->isLoadIssued()){ //Load Not Issued
          ++lsqSquashedLoadsNotIssued;
          //No Cleanup in Caches Required.
          loadQueue[load_idx]->setSquashBuffered(false); //Cleanup Done
        
        } else if(loadQueue[load_idx]->isLoadIssued() && (!loadQueue[load_idx]->isLoadReturned())){ //Load-Inflight
          //Default
          loadQueue[load_idx]->setSquashBuffered(false); //Removed from buffer, default option.
          
          if(cleanupLQInflight_on_Squash){
            assert(loadQueue[load_idx]->cleanupReq != NULL);
            
            bool cleanupReqIssueSuccess = issueCleanupRequest(loadQueue[load_idx],loadsInflightCleanupIssued, true);
            if(cleanupReqIssueSuccess){
              ++loadsInflightCleanupIssued;
              ++lsqSquashedLoadsInflight_Tot;              

            } else{          
              ++skippedCleanupReqs;              
              loadQueue[load_idx]->setCleanupReqSkipped(true);
              loadQueue[load_idx]->setSquashBuffered(true); //Not removed from Buffer - keep it true. Next iteration will take care
              DPRINTF(LSQUnit,"Load CleanupReq Skipped: PC %s, [sn:%lli], Addr:%#x, LineAddr:%#x, LoadIssued:%d, LoadReturned:%d\n",
                      loadQueue[load_idx]->pcState(),
                      loadQueue[load_idx]->seqNum,
                      loadQueue[load_idx]->physEffAddrLow,
                      makeLineAddress(loadQueue[load_idx]->physEffAddrLow),
                      loadQueue[load_idx]->isLoadIssued(),
                      loadQueue[load_idx]->isLoadReturned());


            }
          }
        } else if(loadQueue[load_idx]->isLoadIssued() && loadQueue[load_idx]->isLoadReturned()) { //Load Returned
          //Default
          loadQueue[load_idx]->setSquashBuffered(false); //Removed from Buffer, by default.
        
          if(loadQueue[load_idx]->isL1HitCC()){ //L1-Hit

            if(!loadQueue[load_idx]->isRetStatUpdated())
              ++lsqSquashedLoadsExec_L1Hits;
            //No Cleanup

            //NoInfo
          } else if( (!loadQueue[load_idx]->isL1HitCC()) && \
                     (!loadQueue[load_idx]->isL2HitCC()) && \
                     (!loadQueue[load_idx]->isL2MissCC()) ){
            if(!loadQueue[load_idx]->isRetStatUpdated())
              ++lsqSquashedLoadsExec_NoHitWhere;
            //No Cleanup                            
          }            
          else { //L1-Miss
            if(!loadQueue[load_idx]->isRetStatUpdated()){
              if(loadQueue[load_idx]->isL2MissCC()){ //L2-Miss
                ++lsqSquashedLoadsExec_L2Miss;
              } else if(loadQueue[load_idx]->isL2HitCC()){ //L2-Hit
                ++lsqSquashedLoadsExec_L2Hits;
              } else {
                assert(0);
              }
            }
          
            if(cleanupLQExecd_on_Squash){                
              assert(loadQueue[load_idx]->cleanupReq != NULL);
              
              bool cleanupReqIssueSuccess = issueCleanupRequest(loadQueue[load_idx],loadsExecutedCleanupIssued, false);              
              if(cleanupReqIssueSuccess){
                ++loadsExecutedCleanupIssued;
                ++lsqSquashedLoadsExec_Tot;
              } else{          
                ++skippedCleanupReqs;              
                loadQueue[load_idx]->setCleanupReqSkipped(true);
                loadQueue[load_idx]->setSquashBuffered(true); //Not removed from Buffer in this corner case. Next iteration will take care
                DPRINTF(LSQUnit,"Load CleanupReq Skipped: PC %s, [sn:%lli], Addr:%#x, LineAddr:%#x, LoadIssued:%d, LoadReturned:%d\n",
                        loadQueue[load_idx]->pcState(),
                        loadQueue[load_idx]->seqNum,
                        loadQueue[load_idx]->physEffAddrLow,
                        makeLineAddress(loadQueue[load_idx]->physEffAddrLow),
                        loadQueue[load_idx]->isLoadIssued(),
                        loadQueue[load_idx]->isLoadReturned());
              }
            }
              
          }
          loadQueue[load_idx]->isRetStatUpdated(true);
          //Only concerned about Returned Loads - want to update stat when we first see returned load before squash.
          
        } else {
          assert(0); //Should not reach here.
        }
      }
    }

    --loads_to_squash;
    decrLdIdx(load_idx);
    
  }

  DPRINTF(LSQUnit, "Remaining: %d Skipped CleanupAcks  for Load-Squash till [sn:%lli].\n",
          skippedCleanupReqs,issue_SquashedFromSeqNum);

  DPRINTF(LSQUnit, "For squash [sn:%lli], SquashBuffer: squashbuffer_used:%d. NeedAcks:%d, GotAcks:%d, SkippedReqs:%d  \n",squashfrom_seqnum, squashbuffer_used,loadsInflightCleanupIssued + loadsExecutedCleanupIssued, cleanupAcksReceived,skippedCleanupReqs);

  //Check if Buffer emptied completely ?
  load_idx = loadTail;
  loads_to_squash = loads;
  decrLdIdx(load_idx);
  bool buffer_empty = true;
  while ((loads_to_squash != 0) && (loadQueue[load_idx]->seqNum > issue_SquashedFromSeqNum) ) { 
    if(loadQueue[load_idx]->isSquashTouched() && loadQueue[load_idx]->isSquashBuffered()){
      buffer_empty = false;
      break;
    }
         
    --loads_to_squash;
    decrLdIdx(load_idx);      
  }
     
  if(buffer_empty){
    DPRINTF(LSQUnit, "Done Sending CleanupReq from Buffered LSQ for Load-Squash till [sn:%lli].\n", issue_SquashedFromSeqNum);
    squashbuffer_empty = true;    
    //Actually mark buffer clear in instruction only during clearLoadQueue
  }

  squashbuffer_used = true;  

  DPRINTF(LSQUnit,"**Squash Buffer Empty In Progress  for Waiting for SquashFrom: %lli. squashbuffer_used; %d, squashbuffer_empty: %d\n", \
          squashfrom_seqnum,squashbuffer_used, squashbuffer_empty);

  
  return buffer_empty;
  
}


// [CleanupCache] issue cleanup requests for loads being squashed.
template <class Impl>
bool
LSQUnit<Impl>::issueCleanupRequest(DynInstPtr load_inst, int cleanup_load_id, bool inflight)
{

  //TODO: Need to check if ExecSquash changes anything.
  bool cleanup_issue_success = true;
  
  //Data packet to be issued.
  PacketPtr data_pkt = NULL;
  LSQSenderState *state = new LSQSenderState;
  state->isLoad = false;
  state->idx = cleanup_load_id;
  state->inst = load_inst;
  state->noWB = true;
  state->isSplit = false;

  //Assume no split - TODO for more accurate modelling.
  
  //Cleanup request
  Request *req = load_inst->cleanupReq;
  assert(req != NULL);  
  data_pkt = Packet::createCleanupPkt(req, inflight); 
  data_pkt->senderState = state;
  data_pkt->setFirst();
  data_pkt->reqIdx = cleanup_load_id;

  //Payload for CleanupExecReq
  if(!inflight){
    data_pkt->EvictedLineAddr = load_inst->EvictedLineAddrL1;
    if(load_inst->isL1HitCC()){
      data_pkt->setL1Hit();
    } else {
      if(load_inst->isL2MissCC()){ //L2-Miss
        data_pkt->setL2Miss();
      } else if(load_inst->isL2HitCC()){ //L2-Hit 
        data_pkt->setL2Hit();
      } else {
        assert(0);
      }
    }
    data_pkt->MemCompletionTime = load_inst->MemCompletionTime;
    
  } else {
    data_pkt->EvictedLineAddr = ((Addr)-1);    
  }
  
  if (!dcachePort->sendTimingReq(data_pkt)){
    DPRINTF(IEW, "**CLEANUP** D-Cache became blocked when "
            "validating [sn:%lli], will retry later\n",
            load_inst->seqNum);
    cleanup_issue_success = false;
    delete state;
    delete data_pkt;
    ++lsqCacheBlocked;
  }
  
  if(cleanup_issue_success) {
    load_inst->setCleanupAckNeeded(true);
    DPRINTF(LSQUnit, "**CLEANUP** Cleanup%s  Issued for [Addr:%#x, sn:%d] with Payload L1-Hit:%d, L2-Hit:%d, L2-Miss:%d, RollBackAddrL1: %#x At Time %llu"
            "Inst [sn:%lli] waiting for Ack\n",inflight?"InflightReq":"ExecReq",data_pkt->getAddr(),load_inst->seqNum, data_pkt->isL1Hit(),data_pkt->isL2Hit(),data_pkt->isL2Miss(),data_pkt->EvictedLineAddr,data_pkt->MemCompletionTime,load_inst->seqNum);
  
    load_inst->needDeleteCleanupReq(false); //Destination of this request will delete the Req.    
  }
  
  return cleanup_issue_success;  
}


// [CleanupCache] check if ack received for cleanup requests.

template <class Impl>
    void
    LSQUnit<Impl>::processCleanup(DynInstPtr &inst, PacketPtr pkt){
    cpu->wakeCPU();
    cpu->activityThisCycle();

    assert(inst->isCleanupAckNeeded() == true);
    DPRINTF(LSQUnit, "Received CleanupAck for inst [sn:%lli].\n",
            inst->seqNum);
  
    inst->setCleanupAckNeeded(false); //Ack Received.

    bool cleanup_inflight = (pkt->cmd == MemCmd::CleanupInflightResp);
    if(!cleanup_inflight)
      assert(pkt->cmd == MemCmd::CleanupExecResp);
  
    if(cleanup_inflight){
      assert(!inst->isRetStatUpdated()) ;
      if (pkt->isL1Hit()){      
        inst->setL1HitCC();
        lsqSquashedLoadsInflight_L1Hits++;    
      } else {
        //Means L1Miss
        if(pkt->isL2Hit()){
          inst->setL2HitCC();
          lsqSquashedLoadsInflight_L2Hits++;
        } else if (pkt->isL2Miss()) {
          inst->setL2MissCC();
          lsqSquashedLoadsInflight_L2Miss++;
        } else{
          lsqSquashedLoadsInflight_NoHitWhere++;
        }
      }

      inst->isRetStatUpdated(true);
    }

    
    ++cleanupAcksReceived;
    return;
    
}

//Returns whether load-queue cleared or not - based on whether cleanup acks received.
template <class Impl>
bool 
LSQUnit<Impl>::clearLoadQueue(){
   
  assert(squash_in_progress == true);
  InstSeqNum squashed_num = squashfrom_seqnum;
   
  DPRINTF(LSQUnit, "Waiting for CleanupAcks for Load-Squash till [sn:%lli]. NeedAcks:%d, GotAcks:%d, SkippedReqs:%d, squashbuffer_used:%d, squashbuffer_empty:%d\n",
          squashed_num, loadsInflightCleanupIssued + loadsExecutedCleanupIssued, cleanupAcksReceived,skippedCleanupReqs,squashbuffer_used, squashbuffer_empty);

  //If squashed loads not been insterted into squash buffer yet then cannot clear load queue.
  if(!squashbuffer_used) 
    return false;

  if(!squashbuffer_empty) 
    return false;

  //If squash buffer used and total outstanding cleanupacks > 0, then cannot clear load queue. 
  if(skippedCleanupReqs + loadsInflightCleanupIssued + loadsExecutedCleanupIssued - cleanupAcksReceived > 0 ){  
    return false;
  }

  DPRINTF(LSQUnit, "Finished Wait for CleanupAcks for Load-Squash till [sn:%lli]. NeedAcks:%d, GotAcks:%d\n",
          squashed_num, loadsInflightCleanupIssued + loadsExecutedCleanupIssued, cleanupAcksReceived);

  //All Acks received.
  skippedCleanupReqs = 0;
  loadsInflightCleanupIssued = 0;
  loadsExecutedCleanupIssued = 0;
  cleanupAcksReceived = 0;

  
  //Clear Load Queue
  int load_idx = loadTail;
  decrLdIdx(load_idx);
 
  while (loads != 0 && (loadQueue[load_idx]->seqNum > squashed_num)) {

    assert((!loadQueue[load_idx]->isCleanupAckNeeded()) && (!loadQueue[load_idx]->isCleanupReqSkipped()) \
           && (loadQueue[load_idx]->isSquashTouched()) );

    loadQueue[load_idx]->setRemovedFromSquashBuf(true);
    DPRINTF(LSQUnit, "Removed Load Inst [sn:%lli] from SquashBuffer - isRemovedFromSquashBuf=%d.\n",
            loadQueue[load_idx]->seqNum,loadQueue[load_idx]->isRemovedFromSquashBuf());

    // Clear the smart pointer to make sure it is decremented.    
    loadQueue[load_idx]->setSquashed();
    loadQueue[load_idx] = NULL;
    --loads;

    loadTail = load_idx;
    decrLdIdx(load_idx);
    ++lsqSquashedLoadsTot;
  }
  
  squash_in_progress = false;
  squashbuffer_used = false;
  squashbuffer_empty = false;
  squashbuffer_emptying_started = false;

  //Mark the end of cleanupSquash
  lsqCleanupSquashEndCycle = cpu->curCycle();
  recordCleanupSquashTime(lsqCleanupSquashEndCycle,lsqCleanupSquashStartCycle, lsqCleanupSquashBufferEmptyStartCycle);
  
  DPRINTF(LSQUnit, "Squashing till [sn:%lli]: Done squash_in_progress:%d, squashbuffer_used:%d\n",squashfrom_seqnum, squash_in_progress,squashbuffer_used);

  //Load Queue Finally Cleared.
  return true;
}


//Returns whether load-queue cleared or not - based on whether cleanup acks received.
template <class Impl>
bool 
LSQUnit<Impl>::reissueSkippedCleanupReq(){

  assert(squash_in_progress == true);

  if(skippedCleanupReqs == 0)
    return true;
  
  InstSeqNum squashed_num = squashfrom_seqnum;

  DPRINTF(LSQUnit, "Re-attempting issue of %d Skipped CleanupAcks for Load-Squash till [sn:%lli].\n",
          skippedCleanupReqs,squashed_num);

  int load_idx = loadTail;
  int loads_to_squash = loads;
  decrLdIdx(load_idx);
  while ((loads_to_squash != 0) && (loadQueue[load_idx]->seqNum > squashed_num)) {

    assert(loadQueue[load_idx]->isSquashTouched());
    
    if(loadQueue[load_idx]->isCleanupReqSkipped() ){ //Try and reissue cleanupReq
      if(loadQueue[load_idx]->isLoadReturned() ){ //Load-Executed
        --skippedCleanupReqs;
        loadQueue[load_idx]->setCleanupReqSkipped(false);
        assert(skippedCleanupReqs >= 0);
        bool updateStats = true;
        
        if(cleanupLQExecd_on_Squash){
          
          if( (!loadQueue[load_idx]->isL1HitCC()) && (loadQueue[load_idx]->isL2HitCC() || loadQueue[load_idx]->isL2MissCC()) ){ //L1-Miss Needs Cleanup
            bool cleanupReqIssueSuccess = issueCleanupRequest(loadQueue[load_idx],loadsExecutedCleanupIssued, false);
            if(cleanupReqIssueSuccess){
              ++loadsExecutedCleanupIssued;
              ++lsqSquashedLoadsExec_Tot;
            } else {
              loadQueue[load_idx]->setCleanupReqSkipped(true);
              ++skippedCleanupReqs;
              updateStats = false;
            }
          } else { //No Cleanup needed
            ++lsqSquashedLoadsExec_Tot;
          }
        }
        
        if(updateStats){ //If !cleanupLQExecd_on_Squash || L1-Hit || (L1-Miss && cleanupIssued)
          if(loadQueue[load_idx]->isL1HitCC()){ //L1-Hit
            ++lsqSquashedLoadsExec_L1Hits;
          } else {
            if(loadQueue[load_idx]->isL2MissCC()){ //L2-Miss
              ++lsqSquashedLoadsExec_L2Miss;
            } else if(loadQueue[load_idx]->isL2HitCC()){ //L2-Hit
              ++lsqSquashedLoadsExec_L2Hits;            
            }  else {
              ++lsqSquashedLoadsExec_NoHitWhere;
            }
          }
        }            
      } else if(loadQueue[load_idx]->isLoadIssued() && !(loadQueue[load_idx]->isLoadReturned())){
        bool cleanupReqIssueSuccess = issueCleanupRequest(loadQueue[load_idx],loadsInflightCleanupIssued, true);
        if(cleanupReqIssueSuccess){
          --skippedCleanupReqs;
          loadQueue[load_idx]->setCleanupReqSkipped(false);
          ++loadsInflightCleanupIssued;
          //Update Stats
          ++lsqSquashedLoadsInflight_Tot;  
        }      
      }
    }
    --loads_to_squash;
    decrLdIdx(load_idx);
  }
  
  DPRINTF(LSQUnit, "Remaining: %d Skipped CleanupAcks  for Load-Squash till [sn:%lli].\n",
          skippedCleanupReqs,squashed_num);

  return (skippedCleanupReqs == 0);
}


/** Record statistics for Cleanup Squash Time **/
template <class Impl>
void 
LSQUnit<Impl>::recordCleanupSquashTime(Cycles end, Cycles start, Cycles mid_squashbuffer_waiting){

  assert(end >= start);
  ++lsqCleanupSquashes;
  lsqCleanupSquashTimeCycles += (end -start);
  lsqCleanupSquashTimeHist.sample(end-start);

  lsqCleanupSquashBufferWaitingTimeCycles += (mid_squashbuffer_waiting - start);
  
  Cycles totCleanupSquashTime = end - start;
  Cycles actCleanupSquashTime = end - mid_squashbuffer_waiting;
  
  assert( (end >= mid_squashbuffer_waiting) && 
            (start <= mid_squashbuffer_waiting) ); //Ensure squashbuffer_empty starts after start & before end.

  cleanup_l1hits_exec_delta = lsqSquashedLoadsExec_L1Hits.value() - cleanup_l1hits_exec_start;
  cleanup_l2hits_exec_delta = lsqSquashedLoadsExec_L2Hits.value() - cleanup_l2hits_exec_start;
  cleanup_l2miss_exec_delta = lsqSquashedLoadsExec_L2Miss.value() - cleanup_l2miss_exec_start;
  cleanup_l1hits_inflight_delta = lsqSquashedLoadsInflight_L1Hits.value() - cleanup_l1hits_inflight_start;
  cleanup_l2hits_inflight_delta = lsqSquashedLoadsInflight_L2Hits.value() - cleanup_l2hits_inflight_start;
  cleanup_l2miss_inflight_delta = lsqSquashedLoadsInflight_L2Miss.value() - cleanup_l2miss_inflight_start;
  cleanup_total_delta = lsqSquashedLoadsInflight_Tot.value() + lsqSquashedLoadsExec_Tot.value() - cleanup_total_start ;
  
  lsqActCleanupSquashTimeHist.sample(actCleanupSquashTime);
  lsqActCleanupExecL1MissHist.sample(cleanup_l2hits_exec_delta+cleanup_l2miss_exec_delta);
  
  DPRINTF(CleanupTime, "CleanupSquashTotalTime: %d cycles, CleanupSquashActTime: %d cycles, CleanupReq Total:%d, \n Exec: L1-Hit:%d, L1-Miss:%d (L2-Hit:%d, L2-Miss:%d) \n Inflight: L1-Hit:%d,L1-Miss=%d  (L2-Hit:%d, L2-Miss:%d).\n",
          totCleanupSquashTime, actCleanupSquashTime, cleanup_total_delta,
          cleanup_l1hits_exec_delta,cleanup_l2hits_exec_delta + cleanup_l2miss_exec_delta,cleanup_l2hits_exec_delta,cleanup_l2miss_exec_delta,
          cleanup_l1hits_inflight_delta,cleanup_l2hits_inflight_delta + cleanup_l2miss_inflight_delta,cleanup_l2hits_inflight_delta,cleanup_l2miss_inflight_delta);
}

// Checks for Reordered Load Hit:
//1. if there is a Executed Load in LQ with same Addr
//2. Identifies the earliest such load. (Earliest)
//3. Sees if the SeqNum is After Current Inst. (Reordered)
//4. Earlier Load was not a L1 Hit. (Not L1-Hit)

//Returns:
//Difference in MemAccessTimes
//Marks loadReturnEpoch, MemAccessTime, isHitOnLaterSpecLoad, HitSpecLoadInst

template <class Impl>
Cycles 
LSQUnit<Impl>::profile_reordered_loads(DynInstPtr &inst){
  Cycles diffMemAccessTime = (Cycles)0;
 
  inst->loadReturnEpoch = cpu->curCycle();
    
  DPRINTF(LSQUnit, "1: Testing reordering for Delayed_Load_Queue: sn:%llu \n",
          inst->seqNum);

  if(!inst->isLoadIssued() || inst->isSplitReq() || inst->isSquashed()){
    inst->MemAccessTime =(Cycles) 0;
    inst->isHitOnLaterSpecLoad = false;
    inst->diffMemAccessTime = (Cycles)0;
    return diffMemAccessTime;      
  }

  //Insert this returned load to L2MSHR Spec Inst Q if L2-Miss.
  if(inst->isL2MissCC())
    insertL2MSHRSpecInstQ(inst);
  
  DPRINTF(LSQUnit, "2: Testing reordering for Delayed_Load_Queue: sn:%llu \n",
          inst->seqNum);

  inst->MemAccessTime = inst->loadReturnEpoch - inst->loadIssueEpoch;

  //Get earliest executed load.  
  DynInstPtr earliest_load = inst;

  DPRINTF(LSQUnit, "3: Testing reordering for Delayed_Load_Queue: sn:%llu \n",
          inst->seqNum);

  int lsq_iter = loadHead;
    
  while( lsq_iter != loadTail) {  
    DynInstPtr currLoad = loadQueue[lsq_iter];
    DPRINTF(LSQUnit, "4: Looping Prev Load Ptr:%llu, iter:%d of %d\n",
            currLoad,lsq_iter-loadHead, (loadTail - loadHead)%32);
    if(currLoad){
      //Check matching addr.
      DPRINTF(LSQUnit, "4: Looping Prev Load sn:%llu, iter:%d of %d\n",
              currLoad->seqNum,lsq_iter-loadHead, (loadTail - loadHead)%32);
    
      if (currLoad->effAddrValid() && currLoad->isLoadIssued() && currLoad->isLoadReturned()) {

        if( (currLoad->physEffAddrLow >>6) == (inst->physEffAddrLow >> 6) &&
            ((currLoad->loadReturnEpoch < earliest_load->loadReturnEpoch)) ){
          DPRINTF(LSQUnit, "4: Found Earlier Load sn:%llu,  iter:%d of %d\n",
                  currLoad->seqNum,lsq_iter-loadHead, (loadTail - loadHead)%32);
          earliest_load = currLoad;       
        }
      }
    }
    incrLdIdx(lsq_iter);
  
  }

  DPRINTF(LSQUnit, "5: Loop finished successfully  sn:%llu. Earliest Load is sn:%llu\n",
          inst->seqNum, earliest_load->seqNum);

  DPRINTF(LSQUnit, "5: Earliest load has sn:%llu, isHit: %d, MemAccessTome %d, Issued=%d Returned=%d, IssueEpoch=%llu, ReturnEopch=%llu, isSplit=%d\n",
          earliest_load->seqNum, earliest_load->isL1HitCC(), earliest_load->MemAccessTime,
          earliest_load->isLoadIssued(), earliest_load->isLoadReturned(), earliest_load->loadIssueEpoch,earliest_load->loadReturnEpoch,earliest_load->isSplitReq());
 
  //Candidate Identified
  if( (earliest_load->seqNum > inst->seqNum)
      && (!earliest_load->isL1HitCC())
      && (!earliest_load->isSplitReq())
      && (earliest_load->MemAccessTime > inst->MemAccessTime)){
    inst->isHitOnLaterSpecLoad = true;
    
    if( (earliest_load->MemAccessTime - inst->MemAccessTime) > (Cycles) 0)
            diffMemAccessTime = (earliest_load->MemAccessTime - inst->MemAccessTime);    
  }
  else {
    inst->isHitOnLaterSpecLoad = false;
    inst->diffMemAccessTime = (Cycles)0;
  }
  DPRINTF(LSQUnit, "6: Earliest Load Used Successfully  sn:%llu. Earliest Load is sn:%llu\n",
          inst->seqNum, earliest_load->seqNum);

  inst->diffMemAccessTime = diffMemAccessTime;
  inst->loadDelayedReturnEpoch = inst->loadReturnEpoch + diffMemAccessTime;

  if(diffMemAccessTime)
     DPRINTF(LSQUnit, "Need to Delay sn:%d by %d cycles. CurrEpoch Cycle: %llu, Delayed Epoch Cycle:%llu \n",
             inst->seqNum, inst->diffMemAccessTime, inst->loadReturnEpoch,inst->loadDelayedReturnEpoch);

   return diffMemAccessTime;
}


template <class Impl>
void
LSQUnit<Impl>::complete_reordered_loads(){
  Cycles curr_epoch = cpu->curCycle();
  if(!d_loadQueue.empty()){
    DynInstPtr inst_temp = dynamic_cast<LSQSenderState *>(d_loadQueue.top()->senderState)->inst;
    DPRINTF(LSQUnit, "Top of Delayed_Load_Queue: sn:%llu to be poped at %llu Epoch. Size=%d \n",
            inst_temp->seqNum, inst_temp->loadDelayedReturnEpoch,d_loadQueue.size());
  }

  if(d_loadQueue.empty())
    return;
  PacketPtr pkt = d_loadQueue.top();
  LSQSenderState *state = dynamic_cast<LSQSenderState *>(pkt->senderState);
  DynInstPtr inst = state->inst;
  assert(inst);
  while(inst->loadDelayedReturnEpoch <= curr_epoch){
    assert(inst);
    if(!inst->isSquashed())
      completeDataAccess_Phase2(pkt);
    else{
      delete state;
    }
    if (pkt->delayedDelete){
      delete pkt->req;
      delete pkt;
    }    

    DPRINTF(LSQUnit, "Popping Delayed_Load_Queue: sn:%llu at %llu Epoch. Size=%d \n",
            inst->seqNum, inst->loadDelayedReturnEpoch,d_loadQueue.size());
    d_loadQueue.pop();
    cpu->activityThisCycle();    
    if(d_loadQueue.empty())
      break;
    pkt = d_loadQueue.top();
    state = dynamic_cast<LSQSenderState *>(pkt->senderState);
    inst = state->inst;    
  }
  
}

// [InvisiSpec] This function deals with
// acknowledge response to memory read/write (Phase 2)
template<class Impl>
void
LSQUnit<Impl>::completeDataAccess_Phase2(PacketPtr pkt)
{
  LSQSenderState *state = dynamic_cast<LSQSenderState *>(pkt->senderState);
  DynInstPtr inst = state->inst;
  assert(inst);
  assert(!cpu->switchedOut());
  if (!inst->isSquashed()) {
    if (!state->noWB) {
      // Only loads and store conditionals perform the writeback
      // after receving the response from the memory
      // [mengjia] validation also needs writeback, expose do not need
      assert(inst->isLoad() || inst->isStoreConditional());

      if (!TheISA::HasUnalignedMemAcc || !state->isSplit ||
          !state->isLoad) {
        writeback(inst, pkt);
      } else {
        writeback(inst, state->mainPkt);
      }
    }

    if (inst->isStore()) {
      completeStore(state->idx);
    }

    if (pkt->isValidate() || pkt->isExpose()) {
      completeValidate(inst, pkt);        
    }
  }

  if (TheISA::HasUnalignedMemAcc && state->isSplit && state->isLoad) {
    delete state->mainPkt->req;
    delete state->mainPkt;
  }

  pkt->req->setAccessLatency();
  // probe point, not sure about the mechanism [mengjia]
  cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

  // Not the good place, but we need to fix the memory leakage
  if (pkt->isExpose() || pkt->isValidate()){
    assert(!inst->needDeletePostReq());
    assert(!pkt->isInvalidate());
    delete pkt->req;
  }
  delete state;
}

// [CleanupCache]: BW & Storage Estimation for Lines in Table at L2 for  tracking Window of Speculation.
// 1. When Load Issued, Insert into Priority Queue with 180 cycles hence.
template<class Impl>
void
LSQUnit<Impl>::insertSpecWindowQ(DynInstPtr& inst){
  //inst->specWindowEndEpoch = inst->loadIssueEpoch + (Cycles)((SPEC_WINDOW) - (BUFFER_CYCLES));
  SpecWindowEntry specEntry;
  specEntry.specWindowEndEpoch = inst->loadIssueEpoch + (Cycles)((SPEC_WINDOW) - (BUFFER_CYCLES));
  specEntry.seqNum = inst->seqNum;
  
  //Insert load into SpecWindow Queue
  specWindowQ.push(specEntry); 
}

// 2. Remove when Squash/commit happens (e.g. whenever calc spec window function is called).
template<class Impl>
void
LSQUnit<Impl>::removeSpecWindowQ(DynInstPtr& load_inst){

  std::list<SpecWindowEntry> tempList;

  //Find the entry in specWindowQ
  bool found = false;
  while(!specWindowQ.empty()){
    SpecWindowEntry qtop = specWindowQ.top();
    if(qtop.seqNum != load_inst->seqNum){
      tempList.push_back(qtop);
      specWindowQ.pop();
    } else {
      specWindowQ.pop();
      found = true;
      break;
    }
  }
  assert(found);
  auto list_it = tempList.begin();
  
  //Reinsert into SpecWindowQ
  while(list_it != tempList.end()){
    SpecWindowEntry temp = *list_it;
    specWindowQ.push(temp);
    list_it = tempList.erase(list_it);
  }    
}

// // 3. On every tick, check if there are any at the top of the queue that need to be pushed back by 200 cycles and BW-stat incremented.
template<class Impl>
void
LSQUnit<Impl>::BWmodelSpecWindowQ(){
  Cycles currEpoch = cpu->curCycle();

  if(specWindowQ.empty())
    return;
  
  SpecWindowEntry top_specLQ = specWindowQ.top();
  if (top_specLQ.specWindowEndEpoch > currEpoch){
    return;
  }
    
  while(top_specLQ.specWindowEndEpoch <= currEpoch){
 
    ++specWindowExtensionMsgs;
    
    top_specLQ.specWindowEndEpoch = top_specLQ.specWindowEndEpoch + (Cycles)(SPEC_WINDOW);

    //Extend the window even for L2-MSHR if it is already installed.
    extendL2MSHRSpecInstQ(top_specLQ.seqNum);
    
    specWindowQ.pop();
    specWindowQ.push(top_specLQ);

    top_specLQ = specWindowQ.top();
  }
}


//Inserted when Load Returns and is L2 Install.
template<class Impl>
void
LSQUnit<Impl>::insertL2MSHRSpecInstQ(DynInstPtr& inst){
  SpecWindowEntry specEntry;
  specEntry.specWindowEndEpoch = inst->loadReturnEpoch + (Cycles)((SPEC_WINDOW));
  specEntry.seqNum = inst->seqNum;
  
  //Insert load into SpecWindow Queue
  l2MSHR_SpecInstQ.push(specEntry);

  //Track the max # of this L2 MSHR spec install entries
  if(specWindowL2StorageExtra.value() < l2MSHR_SpecInstQ.size())
    specWindowL2StorageExtra = l2MSHR_SpecInstQ.size();
}

template<class Impl>
void
LSQUnit<Impl>::extendL2MSHRSpecInstQ(InstSeqNum seqNum){

  std::list<SpecWindowEntry> tempList;

  //Find the entry 
  //  bool found = false;
  while(!l2MSHR_SpecInstQ.empty()){
    SpecWindowEntry qtop = l2MSHR_SpecInstQ.top();
    if(qtop.seqNum != seqNum){
      tempList.push_back(qtop);
      l2MSHR_SpecInstQ.pop();
    } else {
      qtop.specWindowEndEpoch = qtop.specWindowEndEpoch + (Cycles)(SPEC_WINDOW);
      tempList.push_back(qtop);
      l2MSHR_SpecInstQ.pop();
      //found = true;
      break;
    }
  }
  
  auto list_it = tempList.begin();
  //Reinsert 
  while(list_it != tempList.end()){
    SpecWindowEntry temp = *list_it;
    l2MSHR_SpecInstQ.push(temp);
    list_it = tempList.erase(list_it);
  }    
}

 //Removed when the queue-top specWindowEndEpoch is ready to pop as per curr tick. 
template<class Impl>
void
LSQUnit<Impl>::removeL2MSHRSpecInstQ(){
  Cycles currEpoch = cpu->curCycle();

  if(l2MSHR_SpecInstQ.empty())
    return;
  
  SpecWindowEntry top_specLQ = l2MSHR_SpecInstQ.top();
  if (top_specLQ.specWindowEndEpoch > currEpoch){
    return;
  }
    
  while(top_specLQ.specWindowEndEpoch <= currEpoch){
    l2MSHR_SpecInstQ.pop();
    if(l2MSHR_SpecInstQ.empty())
      return;    
    top_specLQ = l2MSHR_SpecInstQ.top();
  }
}


#endif//__CPU_O3_LSQ_UNIT_IMPL_HH__


//Storage Estimation:
// 1. When Load Returns, Insert the Line into the Map.
// 2. When Load Committed/Squashed, remove from the Map.
// 3. Track the maximum size of the Map.
