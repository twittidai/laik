#ifdef USE_FABRIC
#include "laik-internal.h"
#include "laik-backend-fabric.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#define HOME_PORT_STR "7777"
#define HOME_PORT 7777
#define PANIC_NZ(a) if ((ret = a)) laik_log(LAIK_LL_Panic, #a " failed: %s", \
    fi_strerror(ret));
const int ll = LAIK_LL_Debug;

void fabric_prepare(Laik_ActionSeq *as);
void fabric_exec(Laik_ActionSeq *as);
void fabric_cleanup(Laik_ActionSeq *as);
void fabric_finalize(Laik_Instance *inst);
bool fabric_log_action(Laik_Action *a);

/* Declare the check_local() function from src/backend_tcp2.c, which we are
 * re-using because I couldn't find a better way to do it with libfabric
 */
bool check_local(char *host);

/* Backend-specific actions */
/* The LibFabric backend uses RMAs for send and receive, which complete
 * asynchronously. This makes it necessary to wait for the completion of the
 * RMAs before proceeding to the next round. */
#define LAIK_AT_FabAsyncRecv	(LAIK_AT_Backend + 0)
#define LAIK_AT_FabRecvWait	(LAIK_AT_Backend + 1)
#define LAIK_AT_FabAsyncSend	(LAIK_AT_Backend + 2)
#define LAIK_AT_FabSendWait	(LAIK_AT_Backend + 3)
#pragma pack(push,1)
typedef Laik_A_BufRecv Laik_A_FabAsyncRecv;
typedef struct {
  Laik_Action h;
  unsigned int count; /* How many CQ reports to wait for */
} Laik_A_FabRecvWait;
typedef Laik_A_BufSend Laik_A_FabAsyncSend;
typedef Laik_A_FabRecvWait Laik_A_FabSendWait;
#pragma pack(pop)

bool fabric_log_action(Laik_Action *a) {
  switch (a->type) {
    case LAIK_AT_FabRecvWait: {
      Laik_A_FabRecvWait *aa = (Laik_A_FabRecvWait*) a;
      laik_log_append("FabRecvWait: count %u", aa->count);
      break;
    }
    case LAIK_AT_FabSendWait: {
      Laik_A_FabSendWait *aa = (Laik_A_FabSendWait*) a;
      laik_log_append("FabSendWait: count %u", aa->count);
      break;
    }
    case LAIK_AT_FabAsyncSend: {
      Laik_A_FabAsyncSend *aa = (Laik_A_FabAsyncSend*) a;
      laik_log_append("FabAsyncSend: from %p, count %d ==> T%d",
                      aa->buf, aa->count, aa->to_rank);
      break;
    }
    case LAIK_AT_FabAsyncRecv: {
      Laik_A_FabAsyncRecv *aa = (Laik_A_FabAsyncRecv*) a;
      laik_log_append("FabAsyncRecv: T%d ==> to %p, count %d",
                      aa->from_rank, aa->buf, aa->count);
      break;
    }
    default: {
      return false;
    }
  }
  return true;
}

/* TODO: Figure out what InstData is needed for */
static struct _InstData {
  int mylid;
  int world_size;
  int addrlen;
} d;
typedef struct _InstData InstData;

static struct fi_info *info;
static struct fid_fabric *fabric;
static struct fid_domain *domain;
static struct fid_ep *ep;
static struct fi_av_attr av_attr = { 0 };
static struct fi_cq_attr cq_attr = {
  .wait_obj	= FI_WAIT_UNSPEC,
/* Format MUST be FI_CQ_FORMAT_DATA or a superset of it, so that remote CQ
 * data is actually awaited. That is described here:
 * https://github.com/ofiwg/libfabric/discussions/9412#discussioncomment-7245799
 */
  .format	= FI_CQ_FORMAT_DATA
};
/* static struct fi_eq_attr eq_attr = { 0 }; */
static struct fid_av *av;
static struct fid_cq *cqr, *cqt; /* Receive and transmit queues */
/* static struct fid_eq *eq; */
static struct fid_mr **mregs = NULL;
int isAsync = 1;
int ret;

static Laik_Backend laik_backend_fabric = {
  .name		= "Libfabric Backend",
  .prepare	= fabric_prepare,
  .exec		= fabric_exec,
  .cleanup	= fabric_cleanup,
  .finalize	= fabric_finalize,
  .log_action	= fabric_log_action
};

Laik_Instance *laik_init_fabric(int *argc, char ***argv) {
  char *str;

  (void) argc;
  (void) argv;

  /* Init logging as "<hostname>:<pid>", like TCP2 does it
   * TODO: Does this make sense or is it better to use some fabric information?
   */
  char hostname[50];
  if (gethostname(hostname, 50)) {
    fprintf(stderr, "Libfabric: cannot get host name\n");
    exit(1);
  }
  char location[70];
  sprintf(location, "%s:%d", hostname, getpid());
  laik_log_init_loc(location);
  /* TODO: Should we log cmdline like TCP2? */

  /* Hints for fi_getinfo() */
  struct fi_info *hints = fi_allocinfo();
  /* TODO: Are these attributes OK? Do we really need FI_MSG? */
  hints->ep_attr->type = FI_EP_RDM;
  hints->caps = FI_MSG | FI_RMA;

  /* Run-time behaviour depending on environment variables */
  str = getenv("LAIK_FABRIC_SYNC");
  if (str) isAsync = !atoi(str);
  laik_log(ll, "RMA mode: %csync", isAsync ? 'a' : ' ');

  /* Choose the first provider that supports RMA and can reach the master node
   * TODO: How to make sure that all nodes chose the same provider?
   */
  str = getenv("LAIK_FABRIC_HOST");
  char *home_host = str ? str : "localhost";
  str = getenv("LAIK_FABRIC_PORT");
  char *home_port_str = str ? str : HOME_PORT_STR;
  int home_port = atoi(home_port_str);
  if (home_port == 0) home_port = HOME_PORT;
  str = getenv("LAIK_SIZE");
  int world_size = str ? atoi(str) : 1;
  if (world_size == 0) world_size = 1;
  ret = fi_getinfo(FI_VERSION(1,21), home_host, NULL, 0,
      hints, &info);
  if (ret || !info) laik_panic("No suitable fabric provider found!");
  laik_log(ll, "Selected fabric \"%s\", domain \"%s\"",
      info->fabric_attr->name, info->domain_attr->name);
  laik_log(ll, "Addressing format is: %d", info->addr_format);
  fi_freeinfo(hints);

  /* Set up address vector */
  PANIC_NZ(fi_fabric(info->fabric_attr, &fabric, NULL));
  PANIC_NZ(fi_domain(fabric, info, &domain, NULL));
  av_attr.type = FI_AV_TABLE;
  av_attr.count = world_size;
  PANIC_NZ(fi_av_open(domain, &av_attr, &av, NULL));

  /* Open the endpoint, bind it to an event queue and a completion queue */
  PANIC_NZ(fi_endpoint(domain, info, &ep, NULL));
  PANIC_NZ(fi_cq_open(domain, &cq_attr, &cqr, NULL));
  PANIC_NZ(fi_cq_open(domain, &cq_attr, &cqt, NULL));
  PANIC_NZ(fi_ep_bind(ep, &av->fid, 0));
  PANIC_NZ(fi_ep_bind(ep, &cqr->fid, FI_RECV));
  PANIC_NZ(fi_ep_bind(ep, &cqt->fid, FI_TRANSMIT));
#if 0
  /* TODO: do we need an event queue for anything? */
  PANIC_NZ(fi_eq_open(fabric, &eq_attr, &eq, NULL));
  PANIC_NZ(fi_ep_bind(ep, &eq->fid, 0)); /* TODO: is 0 OK here? */
#endif
  PANIC_NZ(fi_enable(ep));

  /* Get the address of the endpoint */
  /* TODO: Don't hard-code array length, call fi_getname twice instead */
  char fi_addr[160]; 
  size_t fi_addrlen = 160;
  PANIC_NZ(fi_getname(&ep->fid, fi_addr, &fi_addrlen));
  laik_log(ll, "Got libfabric EP addr of length %zu:", fi_addrlen);
  laik_log_hexdump(ll, fi_addrlen, fi_addr);

  /* Allocate space for list of peers
   * This can't be done earlier because we only get the address size,
   * which depends on protocol, when we call fi_getname() */
  char *peers = calloc(world_size, fi_addrlen);
  if (!peers) laik_panic("Failed to alloc memory");

  /* Sync node addresses over regular sockets, using a similar process as the
   * tcp2 backend, since libfabric features aren't needed here.
   * 
   * This is also recommended in the "Starting Guide for Writing to libfabric",
   * which can be found here:
   * https://www.slideshare.net/JianxinXiong/getting-started-with-libfabric
   */

  /* Get address of home node */
  struct addrinfo sock_hints = {0}, *res;
  sock_hints.ai_family = AF_INET;
  sock_hints.ai_socktype = SOCK_STREAM;
  getaddrinfo(home_host, home_port_str, &sock_hints, &res);
  
  /* Regardless of whether we become master, we need a socket */
  int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sockfd < 0) laik_panic("Failed to create socket");

  /* If home host is localhost, try to become master */
  bool try_master = check_local(home_host);
  bool is_master = 0;
  if (try_master) {
    /* Avoid wait time to bind to same port */
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
          &(int){1}, sizeof(int)) < 0) laik_panic("Cannot set SO_REUSEADDR");
    /* Try binding a socket to the home address */
    laik_log(ll, "Trying to become master");
    is_master = (bind(sockfd, res->ai_addr, res->ai_addrlen) == 0);
  }

  /* Get address list */
  if (is_master) {
    laik_log(ll, "Became master!");
    d.mylid = 0;
    /* Create listening socket on home node */
    if (listen(sockfd, world_size)) laik_panic("Failed to listen on socket");
    /* Insert our own address into address list */
    memcpy(peers, fi_addr, fi_addrlen);
    /* Get addresses of all nodes */
    int fds[world_size-1];
    for (int i = 0; i < world_size - 1; i++) {
      laik_log(ll, "%d out of %d connected...", i, world_size - 1);
      fds[i] = accept(sockfd, NULL, NULL);
      read(fds[i], peers + (i+1) * fi_addrlen, fi_addrlen);
    }
    /* Send assigned number and address list to every non-master node */
    for (int i = 0; i < world_size - 1; i++) {
      int iplus = i + 1;
      write(fds[i], &iplus, sizeof(int));
      write(fds[i], peers, world_size * fi_addrlen);
      close(fds[i]);
    }
  } else {
    laik_log(ll, "Didn't become master!");
    laik_log(ll, "Connecting to:");
    laik_log_hexdump(ll, res->ai_addrlen, res->ai_addr);
    if (connect(sockfd, res->ai_addr, res->ai_addrlen)) {
      laik_log(LAIK_LL_Error, "Failed to connect: %s", strerror(errno));
      exit(1);
    }
    write(sockfd, fi_addr, fi_addrlen);
    read(sockfd, &(d.mylid), sizeof(int));
    read(sockfd, peers, world_size * fi_addrlen);
  }
  close(sockfd);

  ret = fi_av_insert(av, peers, world_size, NULL, 0, NULL);
  if (ret != world_size) laik_panic("Failed to insert addresses into AV");
  free(peers);

  /* Initialize LAIK */
  d.world_size = world_size;
  d.addrlen = fi_addrlen;
  Laik_Instance *inst = laik_new_instance(&laik_backend_fabric, world_size,
      d.mylid, 0, 0, "", &d); /* TODO: what is location? */
  Laik_Group *world = laik_create_group(inst, world_size);
  world->size = world_size;
  world->myid = d.mylid;
  inst->world = world;
  return inst;
}

void add_fabRecvWait(Laik_Action **next, unsigned round, unsigned count) {
  Laik_A_FabRecvWait wait = {
    .h = {
      .type  = LAIK_AT_FabRecvWait,
      .len   = sizeof(Laik_A_FabRecvWait),
      .round = round,
      .tid   = 0,
      .mark  = 0
    },
    .count = count
  };
  memcpy(*next, &wait, sizeof(Laik_A_FabRecvWait));
  *next = nextAction((*next));
}

void add_fabSendWait(Laik_Action **next, unsigned round, unsigned count) {
  Laik_A_FabSendWait wait = {
    .h = {
      .type  = LAIK_AT_FabSendWait,
      .len   = sizeof(Laik_A_FabSendWait),
      .round = round,
      .tid   = 0,
      .mark  = 0
    },
    .count = count
  };
  memcpy(*next, &wait, sizeof(Laik_A_FabSendWait));
  *next = nextAction((*next));
}

/* Registers memory buffers to libfabric, so that they can be accessed by RMA */
/* TODO: consider asynchronous instead of the default synchronous completion
 *       of memory registerations */
void fabric_aseq_RegisterMemory(Laik_ActionSeq *as) {
  /* TODO: Any more efficient way of storing memory registrations?
   *       How much memory does this really waste? Is it better or worse than
   *       introducing a backend-specific "register memory binding" action? */
  mregs = realloc(mregs, (as->actionCount + 1) * sizeof(struct fid_mr*));
  if (!mregs) laik_panic("Failed to alloc memory");

  Laik_TransitionContext *tc = as->context[0];
  int mnum = 0;
  int elemsize = tc->data->elemsize;
  Laik_Action *a = as->action;
  for (unsigned i = 0; i < as->actionCount; i++, a = nextAction(a)) {
    if (a->type == LAIK_AT_BufRecv) {
      Laik_A_BufRecv *aa = (Laik_A_BufRecv*) a;
      int reserve = aa->count * elemsize;
//      printf("REG  %d %p <== %d\n", d.mylid, aa->buf, aa->from_rank);
      laik_log(ll, "Reserving %d * %d = %d bytes\n",
          aa->count, elemsize, reserve);
      PANIC_NZ(fi_mr_reg(
          domain, aa->buf, reserve*aa->count, FI_REMOTE_WRITE,
          0, aa->from_rank, 0, &mregs[mnum++], NULL));
    }
  }
  mregs[mnum] = NULL;
}

/* Creates a new sequence that replaces BufSend and BufRecv with FabAsyncSend
 * and FabAsyncRecv, and adds a FabRmaWait at the end of any round that performs
 * at least one RMA */
void fabric_aseq_SplitAsyncActions(Laik_ActionSeq *as) {
  Laik_Action *newAction = malloc(as->bytesUsed
                                + as->roundCount * sizeof(Laik_A_FabRecvWait)
                                + sizeof(Laik_A_FabSendWait));
  if (!newAction) laik_panic("Failed to alloc memory");
  Laik_Action *nextNewA = newAction;

  int sends = 0, recvs = 0;
  int lastRound = 1;
  unsigned waitCnt = 0;

  Laik_TransitionContext *tc = as->context[0];
  Laik_Action *a = as->action;
  for (unsigned i = 0; i < as->actionCount; i++, a = nextAction(a)) {
    /* Add FabRmaWait that waits for all RMAs of the last round to complete */
    if (a->round != lastRound) {
      if (recvs > 0) {
        add_fabRecvWait(&nextNewA, lastRound, recvs);
        waitCnt++;
      }
      recvs = 0;
      lastRound = a->round;
    }

    /* Count RMAs and copy actions into newAction */
    switch (a->type) {
      case LAIK_AT_BufSend:
        a->type = LAIK_AT_FabAsyncSend;
        sends++;
        break;
      case LAIK_AT_BufRecv:
        a->type = LAIK_AT_FabAsyncRecv;
        recvs++;
        break;
    }
    memcpy(nextNewA, a, a->len);
    nextNewA = nextAction(nextNewA);
  }
  /* Add a FabRmaWait after the final round, too */
  if (recvs > 0) {
    add_fabRecvWait(&nextNewA, lastRound, recvs);
    waitCnt++;
  }

  /* At the end of the action sequence, ensure that all the sends completed */
  add_fabSendWait(&nextNewA, lastRound, sends);

  /* Replace old action sequence with new and update AS information */
  free(as->action);
  as->action       = newAction;
  as->actionCount += waitCnt + 1;
  as->bytesUsed   += waitCnt * sizeof(Laik_A_FabRecvWait)
                  + sizeof(Laik_A_FabSendWait);
}

/* Makes the action sequence asynchronous:
 * - BufSend and BufRecv that are not at the end of a round get replaced by
 *   FabAsyncSend and FabAsyncRecv
 * - BufSend and BufRecv that are at the end of a round stay there, to ensure
 *   synchronization and ordering
 *
 * TODO: This seems not possible, because the last send of one node may not
 * 	 correspond to the last receive of another node. Example:
 *	Node 1		Node 2		Node 3
 *	BufSend -> 2	BufSend -> 0	BufRecv <- 1
 *	BusRecv <- 1	BufSend -> 2	BufRecv <- 0
 * TODO: Are there any optimizations that make this possible? Some of them
 *       seem to affect the order of operations.
 */
#if 0
void fabric_aseq_MakeAsync(Laik_ActionSeq *as) {
  Laik_Action *lastSend
  Laik_Action *a;
  for (unsigned i = 0; i < as->actionCount; i++, a = nextAction(a)) {
    /* Add FabRmaWait that waits for all RMAs of the last round to complete */
  }
}
#endif

void fabric_prepare(Laik_ActionSeq *as) {
  if (as->actionCount == 0) {
    laik_aseq_calc_stats(as);
    return;
  }

  /* Mark action seq as prepared by fabric backend, so that
   * fabric_cleanup() gets invoked for cleanup */
  as->backend = &laik_backend_fabric;

  laik_log_ActionSeqIfChanged(true, as, "Original sequence");
  bool changed = laik_aseq_splitTransitionExecs(as);
  laik_log_ActionSeqIfChanged(changed, as, "After splitting transition execs");
  changed = laik_aseq_flattenPacking(as);
  laik_log_ActionSeqIfChanged(changed, as, "After flattening actions");

  /* TODO: does laik_aseq_replaceWithAllReduce() make sense? */

  /* TODO: This is just copied from src/backend-mpi.c. Do all these
   *       transformations also make sense for libfabric, the same order? */
  changed = laik_aseq_combineActions(as);
  laik_log_ActionSeqIfChanged(changed, as, "After combining actions 1");
  changed = laik_aseq_allocBuffer(as);
  laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 1");
  changed = laik_aseq_splitReduce(as);
  laik_log_ActionSeqIfChanged(changed, as, "After splitting reduce actions");
  changed = laik_aseq_allocBuffer(as);
  laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 2");
  changed = laik_aseq_sort_rounds(as);
  laik_log_ActionSeqIfChanged(changed, as, "After sorting rounds");
  changed = laik_aseq_combineActions(as);
  laik_log_ActionSeqIfChanged(changed, as, "After combining actions 2");
  changed = laik_aseq_allocBuffer(as);
  laik_log_ActionSeqIfChanged(changed, as, "After buffer allocation 3");
  changed = laik_aseq_sort_2phases(as);
  laik_log_ActionSeqIfChanged(changed, as,
      "After sorting for deadlock avoidance");
  laik_aseq_freeTempSpace(as);

  fabric_aseq_RegisterMemory(as);
  if (isAsync) {
    fabric_aseq_SplitAsyncActions(as);
    laik_log_ActionSeqIfChanged(true, as, "After splitting async actions");
  }

  laik_aseq_calc_stats(as);
}

void fabric_exec(Laik_ActionSeq *as) {
  Laik_Action *a = as->action;
  Laik_TransitionContext *tc = as->context[0];
  int elemsize = tc->data->elemsize;
  struct fi_cq_data_entry cq_buf;
  const int cring_size = 8;
  unsigned cring[8] = { 0 };
  int cring_idx = 0;
  for (unsigned i = 0; i < as->actionCount; i++, a = nextAction(a)) {
    switch (a->type) {
      case LAIK_AT_Nop: break;
      case LAIK_AT_FabAsyncRecv: {
        break;
      }
      case LAIK_AT_FabAsyncSend: {
        Laik_A_BufSend *aa = (Laik_A_BufSend*) a;
        while ((ret = fi_writedata(ep, aa->buf, elemsize * aa->count, NULL,
                a->round, aa->to_rank, 0, d.mylid, NULL)) == -FI_EAGAIN);
        if (ret)
          laik_log(LAIK_LL_Panic,
              "fi_writedata() failed: %s", fi_strerror(ret));
        break;
      }
      case LAIK_AT_FabRecvWait: {
        Laik_A_FabRecvWait *aa = (Laik_A_FabRecvWait*) a;
        printf("Waiting for %d recv completions\n", aa->count);
        unsigned completions = cring[cring_idx];
        cring_idx = (cring_idx + 1) % cring_size;
        while (completions < aa->count) {
          ret = fi_cq_sread(cqr, (char*) &cq_buf, 1, NULL, -1);
          if (ret == -FI_EAGAIN) continue;
          assert(ret > 0); /* TODO: actual error handling */
          printf("Got completion for round: %d\n", cq_buf.data);
          if (cq_buf.data == a->round) {
            completions++;
          } else {
            int off = cq_buf.data - a->round - 1;
            assert(off < cring_size);
            cring[(cring_idx + off) % cring_size]++;
          }
        }
        break;
      }
      case LAIK_AT_FabSendWait: {
        Laik_A_FabSendWait *aa = (Laik_A_FabSendWait*) a;
        printf("Waiting for %d send completions\n", aa->count);
        unsigned completions = 0;
        while (completions < aa->count) {
          ret = fi_cq_sread(cqt, (char*) &cq_buf, 1, NULL, -1);
          if (ret == -FI_EAGAIN) continue;
          assert(ret > 0);
          /* TODO: either do something with the retrieved information
           *       or replace the CQ with a counter */
          completions++;
        }
        break;
      }
      /* BufSend and BufRecv only appear if isAsync == 0 */
//#define PFDBG 1
#ifdef PFDBG
#define D(a) a
#else
#define D(a) ;
#endif
      case LAIK_AT_BufRecv: {
        Laik_A_BufRecv *aa = (Laik_A_BufRecv*) a;
        D(printf("RECV %d %p <== %d\n", d.mylid, aa->buf, aa->from_rank));
        while ((ret = fi_cq_sread(cqr, (char*) &cq_buf, 1, NULL, -1))
                                                                 == -FI_EAGAIN);
        assert(ret == 1);
        D(printf("CRCV %d %p <== %d\n", d.mylid, aa->buf, aa->from_rank));
        break;
      }
      case LAIK_AT_BufSend: {
        Laik_A_BufSend *aa = (Laik_A_BufSend*) a;
        D(printf("SEND %d %p ==> %d\n", d.mylid, aa->buf, aa->to_rank));
/* ***********TEST************* */
        const struct iovec msg_iov = {
          .iov_base = aa->buf,
          .iov_len = elemsize * aa->count
        };
        struct fi_rma_iov rma_iov = {
          .addr	= 0,
          .len	= elemsize * aa->count,
          .key	= d.mylid
        };
        struct fi_msg_rma msg = {
          .msg_iov	 = &msg_iov,
          .desc		 = NULL,
          .iov_count	 = 1,
          .addr		 = aa->to_rank,
          .rma_iov	 = &rma_iov,
          .rma_iov_count = 1,
          .context	 = NULL,
          .data		 = 0
        };
        while ((ret = fi_writemsg(ep, &msg, FI_DELIVERY_COMPLETE | FI_FENCE
                                            | FI_REMOTE_CQ_DATA))
               == -FI_EAGAIN);
/* ***********TEST************* */
#if 0
        while ((ret = fi_writedata(ep, aa->buf, elemsize * aa->count, NULL,
                0, aa->to_rank, 0, d.mylid, NULL)) == -FI_EAGAIN);
#endif
        if (ret)
          laik_log(LAIK_LL_Panic,
              "fi_writedata() failed: %s", fi_strerror(ret));
retry:
        while ((ret = fi_cq_sread(cqt, (char*) &cq_buf, 1, NULL, -1))
                                                                 == -FI_EAGAIN);
        if (ret < 0) {
          if (ret != -FI_EAVAIL)
            laik_log(LAIK_LL_Panic, "fi_cq_sread() failed: %s",
                     fi_strerror(ret));
          struct fi_cq_err_entry err;
          if (fi_cq_readerr(cqt, &err, 0) != 1)
            laik_panic("Failed to retrieve error information");

#if 0
          if (err.prov_errno == FI_EINPROGRESS) {
            laik_log(LAIK_LL_Error, "%d: Operation now in progress", d.mylid);
            printf("Retrying: SEND %d %p ==> %d\n", d.mylid, aa->buf, aa->to_rank);

            usleep(500000);
            goto retry;
          }
#endif

          laik_log(LAIK_LL_Panic, "CQ reported error: %s",
              fi_cq_strerror(cqt, err.prov_errno, err.err_data, NULL, 0));
        }
        assert(ret == 1);
        D(printf("CSND %d %p ==> %d\n", d.mylid, aa->buf, aa->to_rank));
        break;
      }
      case LAIK_AT_RBufLocalReduce: {
        Laik_BackendAction *ba = (Laik_BackendAction*) a;
        assert(ba->bufID < ASEQ_BUFFER_MAX);
        assert(ba->dtype->reduce != 0);
        (ba->dtype->reduce)(ba->toBuf, ba->toBuf,
            as->buf[ba->bufID] + ba->offset, ba->count, ba->redOp);
        break;
      }
      default:
        laik_log(LAIK_LL_Error, "Unrecognized action type");
        laik_log_begin(LAIK_LL_Error);
        laik_log_Action(a, as);
        laik_log_flush("");
        exit(1);
    }
  }
}

void fabric_cleanup(Laik_ActionSeq *as) {
  (void) as;
  /* Clean up RDMAs */
  for (struct fid_mr **mr = mregs; *mr; mr++) {
    PANIC_NZ(fi_close((struct fid *) *mr));
  }
}

void fabric_finalize(Laik_Instance *inst) {
  /* TODO: Final cleanup */
  (void) inst;

  free(mregs);
  fi_close((struct fid *) ep);
  fi_close((struct fid *) domain);
  fi_close((struct fid *) fabric);
  fi_freeinfo(info);
}

#endif /* USE_FABRIC */
