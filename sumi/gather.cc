/**
Copyright 2009-2018 National Technology and Engineering Solutions of Sandia, 
LLC (NTESS).  Under the terms of Contract DE-NA-0003525, the U.S.  Government 
retains certain rights in this software.

Sandia National Laboratories is a multimission laboratory managed and operated
by National Technology and Engineering Solutions of Sandia, LLC., a wholly 
owned subsidiary of Honeywell International, Inc., for the U.S. Department of 
Energy's National Nuclear Security Administration under contract DE-NA0003525.

Copyright (c) 2009-2018, NTESS

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Questions? Contact sst-macro-help@sandia.gov
*/

#include <sumi/gather.h>
#include <sumi/communicator.h>
#include <sumi/transport.h>

namespace sumi {

void
BtreeGatherActor::initTree()
{
  log2nproc_ = 0;
  midpoint_ = 1;
  int nproc = comm_->nproc();
  while (midpoint_ < nproc){
    midpoint_ *= 2;
    log2nproc_++;
  }
  //unrull one - we went too far
  midpoint_ /= 2;
}

void
BtreeGatherActor::initBuffers()
{
  void* dst = result_buffer_;
  void* src = send_buffer_;
  if (!src)
    return;

  int me = comm_->myCommRank();
  int nproc = comm_->nproc();

  if (me == root_){
    int buf_size = nproc * nelems_ * type_size_;
    result_buffer_ = my_api_->makePublicBuffer(dst, buf_size);
    recv_buffer_ = result_buffer_;
    send_buffer_ = result_buffer_;
  } else {
    int max_recv_buf_size = midpoint_*nelems_*type_size_;
    recv_buffer_ = my_api_->allocatePublicBuffer(max_recv_buf_size);
    send_buffer_ = recv_buffer_;
    result_buffer_ = recv_buffer_;
  }

  ::memcpy(recv_buffer_, src, nelems_*type_size_);
}

void
BtreeGatherActor::finalizeBuffers()
{
  if (!result_buffer_)
    return;

  int nproc = comm_->nproc();
  int me = comm_->myCommRank();
  if (me == root_){
    int buf_size = nproc * nelems_ * type_size_;
    my_api_->unmakePublicBuffer(result_buffer_, buf_size);
  } else {
    int max_recv_buf_size = midpoint_*nelems_*type_size_;
    my_api_->freePublicBuffer(recv_buffer_,max_recv_buf_size);
  }
}

void
BtreeGatherActor::startShuffle(Action *ac)
{
  if (result_buffer_){
    //only ever arises in weird midpoint scenarios
    int copy_size = ac->nelems * type_size_;
    int copy_offset = ac->offset * type_size_;
    char* dst = ((char*)result_buffer_) + copy_offset;
    char* src = ((char*)result_buffer_);
    ::memcpy(dst, src, copy_size);
  }
}

void
BtreeGatherActor::bufferAction(void *dst_buffer, void *msg_buffer, Action *ac)
{
  std::memcpy(dst_buffer, msg_buffer, ac->nelems * type_size_);
}

void
BtreeGatherActor::initDag()
{
  int me = comm_->myCommRank();
  int nproc = comm_->nproc();
  int round = 0;

  int maxGap = midpoint_;
  if (root_ != 0){
    //special case to handle the last gather round
    maxGap = midpoint_ / 2;
  }

  //as with the allgather, it makes no sense to run the gather on unpacked data
  //everyone should immediately pack all their buffers and then run the collective
  //directly on already packed data

  Action* prev = 0;

  int partnerGap = 1;
  int stride = 2;
  while (1){
    if (partnerGap > maxGap) break;

    //just keep going until you stop
    if (me % stride == 0){
      //I am a recver
      int partner = me + partnerGap;
      if (partner < nproc){
        Action* recv = new RecvAction(round, partner, RecvAction::in_place);
        int recvChunkStart = me + partnerGap;
        int recvChunkStop = std::min(recvChunkStart+partnerGap, nproc);
        int recvChunkSize = recvChunkStop - recvChunkStart;
        recv->nelems = nelems_ * recvChunkSize;
        recv->offset = partnerGap * nelems_;  //I receive into top half of my buffer
        addDependency(prev, recv);
        prev = recv;
      }
    } else {
      //I am a sender
      int partner = me - partnerGap;
      Action* send = new SendAction(round, partner, SendAction::in_place);
      int sendChunkStart = me;
      int sendChunkStop = std::min(sendChunkStart+partnerGap,nproc);
      int sendChunkSize = sendChunkStop - sendChunkStart;
      send->nelems = nelems_*sendChunkSize;
      send->offset = 0; //I send my whole buffer
      addDependency(prev, send);
      prev = send;
      break; //I am done, yo
    }
    ++round;
    partnerGap *= 2;
    stride *= 2;
  }

  round = log2nproc_;
  if (root_ != 0 && root_ == midpoint_ && me == root_){
    //I have to shuffle my data
    Action* shuffle = new ShuffleAction(round, me);
    shuffle->offset = midpoint_ * nelems_;
    shuffle->nelems = (nproc - midpoint_) * nelems_;
    addDependency(prev, shuffle);
    prev = shuffle;
  }


  if (root_ != 0){
    //the root must receive from 0 and midpoint
    if (me == root_){
      int size_1st_half = midpoint_;
      int size_2nd_half = nproc - midpoint_;
      //recv 1st half from 0
      Action* recv = new RecvAction(round, 0, RecvAction::in_place);
      recv->offset = 0;
      recv->nelems = nelems_ * size_1st_half;
      addDependency(prev, recv);
      //recv 2nd half from midpoint - unless I am the midpoint
      if (midpoint_ != root_){
        recv = new RecvAction(round, midpoint_, RecvAction::in_place);
        recv->offset = midpoint_*nelems_;
        recv->nelems = nelems_ * size_2nd_half;
        addDependency(prev, recv);
      }
    }
    //0 must send the first half to the root
    if (me == 0){
      Action* send = new SendAction(round, root_, SendAction::in_place);
      send->offset = 0; //send whole thing
      send->nelems = nelems_*midpoint_;
      addDependency(prev,send);
    }
    //midpoint must send the second half to the root
    //unless it is the root
    if (me == midpoint_ && midpoint_ != root_){
      Action* send = new SendAction(round, root_, SendAction::in_place);
      int size = nproc - midpoint_;
      send->offset = 0;
      send->nelems = nelems_ * size;
      addDependency(prev,send);
    }
  }
}


}
