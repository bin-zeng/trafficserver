/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/****************************************************************************

   OneWayTunnel.cc

   A OneWayTunnel is a module that connects two virtual connections, a
   source vc and a target vc, and copies the data between source and target.

   This class used to be called HttpTunnelVC, but it doesn't seem to have
   anything to do with HTTP, so it has been renamed to OneWayTunnel.
 ****************************************************************************/

#include "P_EventSystem.h"
#include "I_OneWayTunnel.h"

// #define TEST

//////////////////////////////////////////////////////////////////////////////
//
//      OneWayTunnel::OneWayTunnel()
//
//////////////////////////////////////////////////////////////////////////////

ClassAllocator<OneWayTunnel> OneWayTunnelAllocator("OneWayTunnelAllocator");

inline void
transfer_data(MIOBufferAccessor &in_buf, MIOBufferAccessor &out_buf)
{
  ink_release_assert(!"Not Implemented.");

  int64_t n = in_buf.reader()->read_avail();
  int64_t o = out_buf.writer()->write_avail();

  if (n > o)
    n = o;
  if (!n)
    return;
  memcpy(in_buf.reader()->start(), out_buf.writer()->end(), n);
  in_buf.reader()->consume(n);
  out_buf.writer()->fill(n);
}

OneWayTunnel::OneWayTunnel()
  : Continuation(0), vioSource(0), vioTarget(0), cont(0), manipulate_fn(0), n_connections(0), lerrno(0), single_buffer(0),
    close_source(0), close_target(0), tunnel_till_done(0), tunnel_peer(0), free_vcs(true)
{
}

OneWayTunnel *
OneWayTunnel::OneWayTunnel_alloc()
{
  return OneWayTunnelAllocator.alloc();
}

void
OneWayTunnel::OneWayTunnel_free(OneWayTunnel *pOWT)
{
  pOWT->mutex = NULL;
  OneWayTunnelAllocator.free(pOWT);
}

void
OneWayTunnel::SetupTwoWayTunnel(OneWayTunnel *east, OneWayTunnel *west)
{
  // make sure the both use the same mutex
  ink_assert(east->mutex == west->mutex);

  east->tunnel_peer = west;
  west->tunnel_peer = east;
}

OneWayTunnel::~OneWayTunnel()
{
}

OneWayTunnel::OneWayTunnel(Continuation *aCont, Transform_fn aManipulate_fn, bool aclose_source, bool aclose_target)
  : Continuation(aCont ? (ProxyMutex *)aCont->mutex : new_ProxyMutex()), cont(aCont), manipulate_fn(aManipulate_fn),
    n_connections(2), lerrno(0), single_buffer(true), close_source(aclose_source), close_target(aclose_target),
    tunnel_till_done(false), free_vcs(false)
{
  ink_assert(!"This form of OneWayTunnel() constructor not supported");
}

void
OneWayTunnel::init(VConnection *vcSource, VConnection *vcTarget, Continuation *aCont, int size_estimate, ProxyMutex *aMutex,
                   int64_t nbytes, bool asingle_buffer, bool aclose_source, bool aclose_target, Transform_fn aManipulate_fn,
                   int water_mark)
{
  mutex = aCont ? (ProxyMutex *)aCont->mutex : (aMutex ? aMutex : new_ProxyMutex());
  cont = aMutex ? NULL : aCont;
  single_buffer = asingle_buffer;
  manipulate_fn = aManipulate_fn;
  n_connections = 2;
  close_source = aclose_source;
  close_target = aclose_target;
  lerrno = 0;
  tunnel_till_done = (nbytes == TUNNEL_TILL_DONE);

  SET_HANDLER(&OneWayTunnel::startEvent);

  int64_t size_index = 0;

  if (size_estimate)
    size_index = buffer_size_to_index(size_estimate);
  else
    size_index = default_large_iobuffer_size;

  Debug("one_way_tunnel", "buffer size index [%" PRId64 "] [%d]\n", size_index, size_estimate);

  // enqueue read request on vcSource.
  MIOBuffer *buf1 = new_MIOBuffer(size_index);
  MIOBuffer *buf2 = NULL;
  if (single_buffer)
    buf2 = buf1;
  else
    buf2 = new_MIOBuffer(size_index);

  buf1->water_mark = water_mark;

  MUTEX_LOCK(lock, mutex, this_ethread());
  vioSource = vcSource->do_io_read(this, nbytes, buf1);
  vioTarget = vcTarget->do_io_write(this, nbytes, buf2->alloc_reader(), 0);
  ink_assert(vioSource && vioTarget);

  return;
}

void
OneWayTunnel::init(VConnection *vcSource, VConnection *vcTarget, Continuation *aCont, VIO *SourceVio, IOBufferReader *reader,
                   bool aclose_source, bool aclose_target)
{
  (void)vcSource;
  mutex = aCont ? (ProxyMutex *)aCont->mutex : new_ProxyMutex();
  cont = aCont;
  single_buffer = true;
  manipulate_fn = 0;
  n_connections = 2;
  close_source = aclose_source;
  close_target = aclose_target;
  tunnel_till_done = true;

  // Prior to constructing the OneWayTunnel, we initiated a do_io(VIO::READ)
  // on the source VC.  We wish to use the same MIO buffer in the tunnel.

  // do_io() read already posted on vcSource.
  SET_HANDLER(&OneWayTunnel::startEvent);

  SourceVio->set_continuation(this);
  MUTEX_LOCK(lock, mutex, this_ethread());
  vioSource = SourceVio;

  vioTarget = vcTarget->do_io_write(this, TUNNEL_TILL_DONE, reader, 0);
  ink_assert(vioSource && vioTarget);
}

void
OneWayTunnel::init(Continuation *aCont, VIO *SourceVio, VIO *TargetVio, bool aclose_source, bool aclose_target)
{
  mutex = aCont ? (ProxyMutex *)aCont->mutex : new_ProxyMutex();
  cont = aCont;
  single_buffer = true;
  manipulate_fn = 0;
  n_connections = 2;
  close_source = aclose_source;
  close_target = aclose_target;
  tunnel_till_done = true;

  // do_io_read() read already posted on vcSource.
  // do_io_write() already posted on vcTarget
  SET_HANDLER(&OneWayTunnel::startEvent);

  ink_assert(SourceVio && TargetVio);

  SourceVio->set_continuation(this);
  TargetVio->set_continuation(this);
  vioSource = SourceVio;
  vioTarget = TargetVio;
}


void
OneWayTunnel::transform(MIOBufferAccessor &in_buf, MIOBufferAccessor &out_buf)
{
  if (manipulate_fn)
    manipulate_fn(in_buf, out_buf);
  else if (in_buf.writer() != out_buf.writer())
    transfer_data(in_buf, out_buf);
}

//////////////////////////////////////////////////////////////////////////////
//
//      int OneWayTunnel::startEvent()
//
//////////////////////////////////////////////////////////////////////////////

//
// tunnel was invoked with an event
//
int
OneWayTunnel::startEvent(int event, void *data)
{
  VIO *vio = (VIO *)data;
  int ret = VC_EVENT_DONE;
  int result = 0;

#ifdef TEST
  const char *event_origin = (vio == vioSource ? "source" : "target"), *event_name = get_vc_event_name(event);
  printf("OneWayTunnel --- %s received from %s VC\n", event_name, event_origin);
#endif

  if (!vioTarget)
    goto Lerror;

  // handle the event
  //
  switch (event) {
  case ONE_WAY_TUNNEL_EVENT_PEER_CLOSE:
    /* This event is sent out by our peer */
    ink_assert(tunnel_peer);
    tunnel_peer = NULL;
    free_vcs = false;
    goto Ldone;

  case VC_EVENT_READ_READY:
    transform(vioSource->buffer, vioTarget->buffer);
    vioTarget->reenable();
    ret = VC_EVENT_CONT;
    break;

  case VC_EVENT_WRITE_READY:
    if (vioSource)
      vioSource->reenable();
    ret = VC_EVENT_CONT;
    break;

  case VC_EVENT_EOS:
    if (!tunnel_till_done && vio->ntodo())
      goto Lerror;
    if (vio == vioSource) {
      transform(vioSource->buffer, vioTarget->buffer);
      goto Lread_complete;
    } else
      goto Ldone;

  Lread_complete:
  case VC_EVENT_READ_COMPLETE:
    // set write nbytes to the current buffer size
    //
    vioTarget->nbytes = vioTarget->ndone + vioTarget->buffer.reader()->read_avail();
    if (vioTarget->nbytes == vioTarget->ndone)
      goto Ldone;
    vioTarget->reenable();
    if (!tunnel_peer)
      close_source_vio(0);
    break;

  Lerror:
  case VC_EVENT_ERROR:
    lerrno = ((VIO *)data)->vc_server->lerrno;
  case VC_EVENT_INACTIVITY_TIMEOUT:
  case VC_EVENT_ACTIVE_TIMEOUT:
    result = -1;
  Ldone:
  case VC_EVENT_WRITE_COMPLETE:
    if (tunnel_peer) {
      // inform the peer:
      tunnel_peer->startEvent(ONE_WAY_TUNNEL_EVENT_PEER_CLOSE, data);
    }
    close_source_vio(result);
    close_target_vio(result);
    connection_closed(result);
    break;

  default:
    ink_assert(!"bad case");
    ret = VC_EVENT_CONT;
    break;
  }
#ifdef TEST
  printf("    (OneWayTunnel returning value: %s)\n", (ret == VC_EVENT_DONE ? "VC_EVENT_DONE" : "VC_EVENT_CONT"));
#endif
  return ret;
}

// If result is Non-zero, the vc should be aborted.
void
OneWayTunnel::close_source_vio(int result)
{
  if (vioSource) {
    if (last_connection() || !single_buffer) {
      free_MIOBuffer(vioSource->buffer.writer());
      vioSource->buffer.clear();
    }
    if (close_source && free_vcs) {
      vioSource->vc_server->do_io_close(result ? lerrno : -1);
    }
    vioSource = NULL;
    n_connections--;
  }
}

void
OneWayTunnel::close_target_vio(int result, VIO *vio)
{
  (void)vio;
  if (vioTarget) {
    if (last_connection() || !single_buffer) {
      free_MIOBuffer(vioTarget->buffer.writer());
      vioTarget->buffer.clear();
    }
    if (close_target && free_vcs) {
      vioTarget->vc_server->do_io_close(result ? lerrno : -1);
    }
    vioTarget = NULL;
    n_connections--;
  }
}

//////////////////////////////////////////////////////////////////////////////
//
//      void OneWayTunnel::connection_closed
//
//////////////////////////////////////////////////////////////////////////////
void
OneWayTunnel::connection_closed(int result)
{
  if (cont) {
#ifdef TEST
    cout << "OneWayTunnel::connection_closed() ... calling cont" << endl;
#endif
    cont->handleEvent(result ? VC_EVENT_ERROR : VC_EVENT_EOS, cont);
  } else {
    OneWayTunnel_free(this);
  }
}

void
OneWayTunnel::reenable_all()
{
  if (vioSource)
    vioSource->reenable();
  if (vioTarget)
    vioTarget->reenable();
}

bool
OneWayTunnel::last_connection()
{
  return n_connections == 1;
}
