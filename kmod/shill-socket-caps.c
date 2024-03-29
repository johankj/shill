#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/systm.h>
#include <sys/ucred.h>
#include <sys/queue.h>
#include <sys/vnode.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <vm/uma.h>

#include <sys/types.h>
#include <sys/socket.h> /* for AF_INET, AF_LOCAL */

#include <security/mac/mac_policy.h>
#include <sys/mac.h>

#include <sys/param.h> /* for KASSERT */
#include <sys/systm.h> /* for KASSERT */
#include <sys/socketvar.h> /* for accesing fields of `struct socket' */
#include <sys/protosw.h> /* for accessing fields of `struct protosw' */
#include <sys/domain.h> /* for accessing fields of `struct domain' */
#include <netinet/in.h> /* for accessing fields of `struct sockaddr_in' */
#include <sys/proc.h> /* for accessing fields of `struct thread' */


#include "shilldev.h" /* for shilld_log, etc. */
#include "shill.h"
#include "shill-socket-caps.h"
#include "mem-manage.h"

/******************************************************************************
 * Memory Management
 */

extern int shill_slot; /* comes from shill.c */

static uma_zone_t network_cap_zone;
static uma_zone_t network_cap_list_zone;
static uma_zone_t socket_original_data_zone;
static uma_zone_t socket_original_data_list_zone;

extern int network_cap_count;
extern int network_cap_list_count;
extern int socket_original_data_count;
extern int socket_original_data_list_count;

DEFINE_ALLOC(shill_network_cap,
             network_cap_zone,
             network_cap_count,
             M_WAITOK)
DEFINE_FREE(shill_network_cap,
            network_cap_zone,
            network_cap_count)
DEFINE_ALLOC(shill_network_cap_list,
             network_cap_list_zone,
             network_cap_list_count,
             M_NOWAIT)
DEFINE_FREE(shill_network_cap_list,
            network_cap_list_zone,
            network_cap_list_count)

DEFINE_ALLOC(shill_socket_original_data,
             socket_original_data_zone,
             socket_original_data_count,
             M_WAITOK)
DEFINE_FREE(shill_socket_original_data,
            socket_original_data_zone,
            socket_original_data_count)
DEFINE_ALLOC(shill_socket_original_data_list,
             socket_original_data_list_zone,
             socket_original_data_list_count,
             M_NOWAIT)
DEFINE_FREE(shill_socket_original_data_list,
            socket_original_data_list_zone,
            socket_original_data_list_count)

MALLOC_DEFINE(M_SHILLPROXYADDR, "shillproxyaddr", "For allocation of proxy address in Shill");


inline void
shill_clear_network_cap_session_list(struct shill_network_cap_list *list) {
  struct shill_network_cap *cur, *cur_temp;
  LIST_FOREACH_SAFE(cur, list, snc_session_list, cur_temp) {
    LIST_REMOVE(cur, snc_session_list);
    shill_network_cap_free(cur);
  }
}

static inline void
shill_clear_network_cap_socket_list(struct shill_network_cap_list *list) {
  struct shill_network_cap *cur, *cur_temp;
  LIST_FOREACH_SAFE(cur, list, snc_socket_list, cur_temp) {
    LIST_REMOVE(cur, snc_socket_list);
    shill_network_cap_free(cur);
  }
}

inline void
shill_clear_session_socket_original_data_list(struct shill_socket_original_data_list *list) {
  struct shill_socket_original_data *cur, *cur_temp;
  LIST_FOREACH_SAFE(cur, list, ssod_list, cur_temp) {
    LIST_REMOVE(cur, ssod_list);
    shill_socket_original_data_free(cur);
  }
}

void
shill_socket_module_init(void) {
  network_cap_zone = uma_zcreate("mac_shill_network_cap",
                                 sizeof(struct shill_network_cap),
                                 NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
  network_cap_list_zone = uma_zcreate("mac_shill_network_cap_list",
                                      sizeof(struct shill_network_cap_list),
                                      NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);

  socket_original_data_zone = uma_zcreate("mac_shill_socket_original_data",
					  sizeof(struct shill_socket_original_data),
					  NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
  socket_original_data_list_zone = uma_zcreate("mac_shill_socket_original_data_list",
					       sizeof(struct shill_socket_original_data_list),
					       NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
}

void
shill_socket_module_destroy(void) {
  uma_zdestroy(network_cap_zone);
  uma_zdestroy(network_cap_list_zone);
  uma_zdestroy(socket_original_data_zone);
  uma_zdestroy(socket_original_data_list_zone);
}

static inline bool is_tcp_proxy_enabled(struct shill_session *session) {
  return session->proxy_host != NULL;
}

static inline bool is_udp_proxy_enabled(struct shill_session *session) {
  return session->dns_host != NULL;
}


/******************************************************************************
 * Copying, Construction, Accessors
 */

static inline struct shill_network_cap *
shill_make_network_cap(int af,
                       int type,
                       uint64_t permissions,
                       struct shill_session *session) {
  struct shill_network_cap * network_cap =
    shill_network_cap_alloc();
  network_cap->snc_af_family = af;
  network_cap->snc_socket_type = type;
  network_cap->snc_permissions = permissions;
  network_cap->snc_session = session;

  return network_cap;
}

static inline struct shill_network_cap *
shill_copy_network_cap(struct shill_network_cap *network_cap) {
  return shill_make_network_cap(network_cap->snc_af_family,
                                network_cap->snc_socket_type,
                                network_cap->snc_permissions,
                                network_cap->snc_session);
}

static inline void
shill_register_cap_with_session(struct shill_session *session,
                                struct shill_network_cap *network_cap) {
  LIST_INSERT_HEAD(&session->ss_network_cap_list,
                   network_cap,
                   snc_session_list);
}

static inline void
shill_network_cap_add_permissions(struct shill_network_cap * network_cap,
                                  uint64_t permissions) {
  network_cap->snc_permissions |= permissions;
}

/* Socket original data */
static inline struct shill_socket_original_data *
shill_make_socket_original_data(in_addr_t ip,
                                unsigned short port,
                                short nonblocking_flag,
                                struct socket *so) {
  struct shill_socket_original_data * sod =
    shill_socket_original_data_alloc();
  sod->ip = ip;
  sod->port = port;
  sod->nonblocking_flag = nonblocking_flag;
  sod->so = so;

  return sod;
}

static inline void
shill_register_socket_original_data_with_session(struct shill_session *session,
                                                 struct shill_socket_original_data *socket_original_data) {
  LIST_INSERT_HEAD(&session->ss_original_data_list,
                   socket_original_data,
                   ssod_list);
}

/******************************************************************************
 * Printing Procedures
 */

static inline void
shill_log_network_cap(struct shill_session *session,
                      struct shill_network_cap *network_cap) {
  shilld_log((uintptr_t)session,
             "network_cap{ af := %d, socket_type := %d, permissions := %lx, "
             "session := %lu }",
             network_cap->snc_af_family,
             network_cap->snc_socket_type,
             network_cap->snc_permissions,
             (uintptr_t)network_cap->snc_session);
}

static inline void
shill_logall_network_cap(struct shill_network_cap *network_cap) {
  shilld_logall("network_cap{ af := %d, permissions := %lx, session := %lu }",
                network_cap->snc_af_family,
                network_cap->snc_permissions,
                (uintptr_t)network_cap->snc_session);
}


static inline void
shill_log_all_network_caps_in_session(struct shill_session *session) {
  struct shill_network_cap_list *list = &session->ss_network_cap_list;
  struct shill_network_cap *cur;
  KASSERT(list != NULL, ("shill_log_all_network_caps_in_session: null list"));
  LIST_FOREACH(cur, list, snc_session_list) {
    shill_log_network_cap(session, cur);
  }
}

static inline struct shill_network_cap_list *
shill_socket_get_caps(struct socket *so);

static inline void
shill_log_all_network_caps_in_socket_for_session(struct shill_session *session,
                                                 struct socket *so) {
  struct shill_network_cap_list *list = shill_socket_get_caps(so);
  struct shill_network_cap *cur;
  KASSERT(list != NULL, ("shill_log_all_network_caps_in_socket_for_session: null list"));
  LIST_FOREACH(cur, list, snc_socket_list) {
    if (cur->snc_session == session) {
      shill_log_network_cap(session, cur);
    }
  }
}

/******************************************************************************
 * System Calls
 */

static inline char
shill_network_cap_lte_any_in_session(struct shill_network_cap *a,
                                     struct shill_session *session);

int
shill_socket_module_syscall(struct ucred *cred,
                            int call,
                            void *user_arg) {
  struct shill_session *session = extract_shill_session(cred->cr_label);
  struct shill_session *parent = NULL;

  if(session == NULL) {
    shilld_logall("Granting socket capabilities not permitted: no current sandbox");
    return EINVAL;
  }

  shill_lock_session(session);
  if (session->ss_state != SESSION_INIT) {
    shill_unlock_session(session);
    shilld_log((uintptr_t)session,
               "Granting socket capabilities not permitted: sandbox already entered");
    return EACCES;
  } else if (session->ss_parent != NULL) {
    parent = session->ss_parent;
  }
  shill_unlock_session(session);

  uint64_t buf[3] = {0,0,0};
  uint64_t address_family;
  uint64_t socket_type;
  uint64_t permissions;
  struct shill_network_cap *new_cap;

  if(copyin(((uint64_t*)user_arg), &buf, sizeof(uint64_t) * 3)) {
    shilld_log((uintptr_t)session,
               "Could not copy user arguments into kernel memory");
    return EFAULT;
  }

  address_family = buf[0];
  socket_type = buf[1];
  permissions = buf[2];

  switch (call) {
  case 1:
    /* add network capability to session.
     *
     * arg points to a three element uint64_t array which corresponds to the
     * address_family, socket_type, and the permissions of the new network_cap
     **/
    shill_lock_session(session);
    new_cap = shill_make_network_cap(address_family,
                                     socket_type,
                                     permissions,
                                     session);
    if (parent != NULL &&
        !shill_network_cap_lte_any_in_session(new_cap, parent)) {
      shilld_log((uintptr_t)session,
                 "Cannot grant network capability because parent session "
                 "doesn't have a super-capability.");
      shill_log_network_cap(session, new_cap);
      shill_network_cap_free(new_cap);
      return EACCES;
    }
    shill_register_cap_with_session(session, new_cap);
    shill_unlock_session(session);

    return 0;
    break;
  default:
    shilld_log((uintptr_t)session,
               "Unsupported socket module system call number, given: %d",
               call);
    return EDOOFUS;
  }
}

int
shill_proxy_module_syscall(struct ucred *cred,
                            int call,
                            void *user_arg) {
  struct shill_session *session = extract_shill_session(cred->cr_label);
  struct shill_session *parent = NULL;

  if(session == NULL) {
    shilld_logall("Granting proxy capabilities not permitted: no current sandbox");
    return EINVAL;
  }

  shill_lock_session(session);
  if (session->ss_state != SESSION_INIT) {
    shill_unlock_session(session);
    shilld_log((uintptr_t)session,
               "Granting proxy capabilities not permitted: sandbox already entered");
    return EACCES;
  } else if (session->ss_parent != NULL) {
    parent = session->ss_parent;
  }
  shill_unlock_session(session);

  uint64_t buf[4] = {0};
  uint64_t proxy_port;
  uint64_t dns_port;

  if(copyin(((uint64_t*)user_arg), &buf, sizeof(uint64_t) * 4)) {
    shilld_log((uintptr_t)session,
               "Could not copy user arguments into kernel memory");
    return EFAULT;
  }

  struct in_addr *proxy_addr = malloc(sizeof(struct in_addr), M_SHILLPROXYADDR, M_WAITOK);
  proxy_addr->s_addr = (in_addr_t) buf[0];
  proxy_port = buf[1];
  struct in_addr *dns_addr = malloc(sizeof(struct in_addr), M_SHILLPROXYADDR, M_WAITOK);
  dns_addr->s_addr = (in_addr_t) buf[2];
  dns_port = buf[3];

  switch (call) {
  case 4:
    shilld_log((uintptr_t)session, "proxy_addr: %x", proxy_addr->s_addr);
    shilld_log((uintptr_t)session, "proxy_port: %i", (int)proxy_port);
    shilld_log((uintptr_t)session, "dns_addr: %x", dns_addr->s_addr);
    shilld_log((uintptr_t)session, "dns_port: %i", (int)dns_port);

    /* add proxy capability to session.
     *
     * arg points to a three element uint64_t array which corresponds to the
     * address_family, socket_type, and the permissions of the new network_cap
     **/
    shill_lock_session(session);

    if (proxy_addr->s_addr != 0) {
      session->proxy_host = proxy_addr;
      session->proxy_port = proxy_port;
    }
    if (dns_addr->s_addr != 0) {
      session->dns_host = dns_addr;
      session->dns_port = dns_port;
    }

    shill_unlock_session(session);

    return 0;
    break;
  default:
    shilld_log((uintptr_t)session,
               "Unsupported proxy module system call number, given: %d",
               call);
    return EDOOFUS;
  }
}




/******************************************************************************
 * Socket Library Wrapper Procedures
 */

static inline int
shill_socket_get_af(struct socket *so) {
  return so->so_proto->pr_domain->dom_family;
}

static inline int
shill_socket_get_socket_type(struct socket *so) {
  return so->so_type;
}


static inline struct label *
shill_socket_get_label(struct socket *so) {
  return so->so_label;
}

/*
 * This method should only be applied to socket labels; otherwise this will be
 * an unsafe cast.
 */
static inline struct shill_network_cap_list *
shill_get_network_cap_list_from_socket_label(struct label *label) {
  return (struct shill_network_cap_list *)mac_label_get(label, shill_slot);
}

static inline struct shill_network_cap_list *
shill_socket_get_caps(struct socket *so) {
  return shill_get_network_cap_list_from_socket_label(shill_socket_get_label(so));
}

/******************************************************************************
 * Lattice Procedures
 */

static inline char
shill_session_has_capability_to_create_in_af(struct shill_session *session,
                                             int af,
                                             int type) {
  /* We create a network cap with no permissions. As a result, all network
   * caps in the same address family will be lte this network cap.
   */
  struct shill_network_cap network_cap;
  network_cap.snc_permissions = 0;
  network_cap.snc_af_family = af;
  network_cap.snc_socket_type = type;

  return shill_network_cap_lte_any_in_session(&network_cap, session);
}

static inline char
shill_network_cap_lte(struct shill_network_cap *a, struct shill_network_cap *b) {
  char comparable_p = a->snc_af_family == b->snc_af_family &&
                      a->snc_socket_type == b->snc_socket_type;
  uint64_t permissions_lte_p = (a->snc_permissions & b->snc_permissions) == a->snc_permissions;
  return comparable_p && permissions_lte_p;
}

static inline char
shill_network_cap_lte_any_in_session(struct shill_network_cap *a, struct shill_session *session) {
  struct shill_network_cap_list *list = &session->ss_network_cap_list;
  struct shill_network_cap *cur;
  KASSERT(list != NULL, ("shill_network_cap_lte_any_in_session: null list"));
  LIST_FOREACH(cur, list, snc_session_list) {
    if(shill_network_cap_lte(a, cur)) {
      return 1;
    }
  }
  return 0; /* we're doing an ormap, so the identity element is false */
}

static inline char
shill_network_cap_lte_any_in_socket_for_session(struct shill_network_cap *a,
                                                struct socket *so,
                                                struct shill_session *session) {
  struct shill_network_cap_list *list = shill_socket_get_caps(so);
  struct shill_network_cap *cur;
  KASSERT(list != NULL, ("shill_network_cap_lte_any_in_socket_for_session: null list"));
  LIST_FOREACH(cur, list, snc_socket_list) {
    if(cur->snc_session == session && shill_network_cap_lte(a, cur)) {
      return 1;
    }
  }
  return 0; /* we're doing an ormap, so the identity element is false */
}

inline static char
shill_network_permissions_lte(uint64_t a, uint64_t b) {
  uint64_t permissions_which_b_has_and_a_does_not = ~a & b;
  char b_has_no_permissions_which_a_does_not_p =
    permissions_which_b_has_and_a_does_not == 0;
  return b_has_no_permissions_which_a_does_not_p;
}

/*
 * MAC Procedures
 */

static inline int
shill_network_cap_check_direct(struct ucred *cred,
                               struct socket *so,
                               const char *name,
                               struct shill_network_cap *required_cap);

/*
 * This is a convenience function which creates takes the permissions instead of
 * a fully created capability.
 */
static inline int
shill_network_cap_check(struct ucred *cred,
                        struct socket *so,
                        const char *name,
                        uint64_t socket_permissions) {
  struct shill_network_cap network_cap;
  network_cap.snc_permissions = socket_permissions;
  network_cap.snc_af_family = shill_socket_get_af(so);
  network_cap.snc_socket_type = shill_socket_get_socket_type(so);

  return shill_network_cap_check_direct(cred, so, name, &network_cap);
}

/*
 * This function is used when the required socket capability already exists. In
 * other circumstances use `shill_network_cap_check'
 */
static inline int
shill_network_cap_check_direct(struct ucred *cred,
                               struct socket *so,
                               const char *name,
                               struct shill_network_cap *required_cap) {
  struct shill_session *session = shill_get_session(cred);
  if (session == NULL)
    return 0;

  shilld_log((uintptr_t)session, "checking socket");

  if (session->ss_state == SESSION_DEBUG) {
    shilld_log((uintptr_t)session,
               "Permitting socket %s for debugging",
               name);
    return 0;
  }

  if (0 == shill_network_cap_lte_any_in_socket_for_session(required_cap,
                                                           so,
                                                           session)) {
    shilld_log((uintptr_t)session,
               "Insufficient capabilities for %s.",
               name);
    return EACCES;
  }

  struct shill_network_cap_list *list = shill_socket_get_caps(so);
  struct shill_network_cap *cur, *cur_temp;
  LIST_FOREACH_SAFE(cur, list, snc_socket_list, cur_temp) {
    if (cur->snc_session == session &&
        !shill_network_cap_lte(required_cap, cur)) {
      shilld_log((uintptr_t)session,
                 "Removing the following capability from socket %lu "
                 "because it is inconsistent with %s.",
                 (uintptr_t)so,
                 name);
      shill_log_network_cap(session, cur);
      LIST_REMOVE(cur, snc_socket_list);
    }
  }
  return 0;
}

int
shill_socket_check_accept(struct ucred *cred,
                          struct socket *so,
                          struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_accept",
                                 SHILL_SOCKET_CAP_ACCEPT);
}

int
shill_socket_check_bind(struct ucred *cred,
                        struct socket *so,
                        struct label *solabel,
                        struct sockaddr *sa) {
  return shill_network_cap_check(cred, so, "check_bind",
                                 SHILL_SOCKET_CAP_BIND);
}

int
shill_socket_check_connect(struct ucred *cred,
                           struct socket *so,
                           struct label *solabel,
                           struct sockaddr *sa) {

  struct shill_session *session = shill_get_session(cred);
  if (session == NULL)
    return 0;

  if (is_udp_proxy_enabled(session) && sa->sa_family == AF_INET && so->so_type == SOCK_DGRAM) {
    if (((struct sockaddr_in *) sa)->sin_port != htons(53)) {
      shilld_log((uintptr_t)session, "Dropping UDP packets for check_connect.");
      return EACCES;
    }
  }

  return shill_network_cap_check(cred, so, "check_connect",
                                 SHILL_SOCKET_CAP_CONNECT);
}


int
shill_socket_before_connect(struct thread *td,
                            struct socket *so,
                            struct label *solabel,
                            struct sockaddr *sa) {
  struct shill_session *session = shill_get_session(td->td_ucred);
  if (session == NULL)
    return 0;

  if (is_udp_proxy_enabled(session) && sa->sa_family == AF_INET && so->so_type == SOCK_DGRAM) {
      struct sockaddr_in *sa_in = (struct sockaddr_in *) sa;
      // Save original IP and port
      struct shill_socket_original_data *ss_od = shill_make_socket_original_data(sa_in->sin_addr.s_addr, sa_in->sin_port, 0, so);
      shill_register_socket_original_data_with_session(session, ss_od);

      if (sa_in->sin_port == htons(53)) {
          shilld_log((uintptr_t)session, "proxy'ing DNS packets");
          sa_in->sin_addr.s_addr = session->dns_host->s_addr;
          sa_in->sin_port = htons(session->dns_port);
      }
  } else
  if (is_tcp_proxy_enabled(session) && sa->sa_family == AF_INET && so->so_type == SOCK_STREAM) {
    shilld_log((uintptr_t)session, "proxy'ing a session");

    struct sockaddr_in *sa_in = (struct sockaddr_in *) sa;

    // Save original IP and port and nonblocking flag
    struct shill_socket_original_data *ss_od = shill_make_socket_original_data(sa_in->sin_addr.s_addr, sa_in->sin_port, so->so_state & SS_NBIO, so);
    shill_register_socket_original_data_with_session(session, ss_od);

    // Overwrite socket address with proxy IP and port
    sa_in->sin_addr.s_addr = session->proxy_host->s_addr;
    sa_in->sin_port = htons(session->proxy_port);

    so->so_state &= ~SS_NBIO; // Force socket to be blocking

    return 1;
  }

  return 0;
}

int
socket_send(struct thread* td,
            struct socket* so,
            unsigned char *buf,
            int len) {
  struct uio auio;
  struct iovec aiov;
  enum uio_seg segflg = UIO_SYSSPACE;
  int error;

  aiov.iov_base = buf;
  aiov.iov_len = len;

  auio.uio_iov = &aiov;
  auio.uio_iovcnt = 1;
  auio.uio_segflg = segflg;
  auio.uio_rw = UIO_WRITE;
  auio.uio_td = td;
  auio.uio_offset = 0;
  auio.uio_resid = len; 	/* sum of all data segments in the iovec */

  struct sockaddr *msg_name = NULL;
  struct mbuf *control = NULL;
  int flags = 0;
  error = sosend(so, msg_name, &auio, 0, control, flags, td);

  return error;
}

int
socket_receive(struct thread* td,
               struct socket* so,
               unsigned char *buf,
               int len) {
  struct uio auio;
  struct iovec aiov;
  enum uio_seg segflg = UIO_SYSSPACE;
  int error;

  aiov.iov_base = buf;
  aiov.iov_len = len;

  auio.uio_iov = &aiov;
  auio.uio_iovcnt = 1;
  auio.uio_segflg = segflg;
  auio.uio_rw = UIO_READ;
  auio.uio_td = td;
  auio.uio_offset = 0;
  auio.uio_resid = len; 	/* sum of all data segments in the iovec */

  struct sockaddr *fromsa = NULL;
  struct mbuf *control = NULL;
  int flags = 0;
  error = soreceive(so, &fromsa, &auio, (struct mbuf **)0, &control, &flags);

  return error;
}


int
shill_socket_after_connect(struct thread *td,
                           struct socket *so,
                           struct label *solabel,
                           struct sockaddr *sa) {
  struct shill_session *session = shill_get_session(td->td_ucred);
  if (session == NULL)
    return 0;

  if (session->proxy_host == NULL)
    return 0;

  // IPv4 and TCP
  if (sa->sa_family == AF_INET && so->so_type == SOCK_STREAM) {

    // Find original address information for proxying
    struct shill_socket_original_data_list *list = &session->ss_original_data_list;
    struct shill_socket_original_data *cur, *cur_temp;
    LIST_FOREACH_SAFE(cur, list, ssod_list, cur_temp) {
      if (cur->so == so) 	/* pointer equality */
	break;
    }
    if (cur == NULL)
      return 0;

    // If not connected, still remove it.
    if ((so->so_state & SS_ISCONNECTED) != SS_ISCONNECTED) {
      printf("shilld: Socket not connected for unknown reasons. Cannot proxy.\n");
      LIST_REMOVE(cur, ssod_list);
      return 1;
    }

    shilld_log((uintptr_t)session,
	       "AFTER CONNECT: Found original data: (%i:%i)\n",
	       cur->ip, cur->port);

    /* Send SOCKSv5 proxy handshake */
    int error = 0;

    // SOCKSv5 connect without authentication
    unsigned char buf1[3] = {0x05, 0x01, 0x00};
    error = socket_send(td, so, buf1, 3);
    if (error)
      printf("AFTER_CONNECT: send buf1 err: %i\n", error);

    unsigned char buf2[2] = {0};
    error = socket_receive(td, so, buf2, 2);
    if (error != 0 || buf2[0] != 5 || buf2[1] != 0)
      printf("AFTER_CONNECT: receive buf2 err: %i\n", error);

    unsigned char buf3[] = {
      05 /*socks*/, 01 /*connect*/, 00 /*reserved*/, 01 /*IPv4*/,
      0x00, 0x00, 0x00, 0x00,  /* remote IP */
      0x00, 0x00  /* remote port */
    };
    buf3[4] = cur->ip & 0xFF;
    buf3[5] = (cur->ip >> 8) & 0xFF;
    buf3[6] = (cur->ip >> 16) & 0xFF;
    buf3[7] = (cur->ip >> 24) & 0xFF;
    buf3[8] = cur->port & 0xFF;
    buf3[9] = (cur->port >> 8) & 0xFF;
    error = socket_send(td, so, buf3, 10);
    if (error)
      printf("AFTER_CONNECT: send buf3 err: %i\n", error);

    unsigned char buf4[10] = {0};
    error = socket_receive(td, so, buf4, 10);
    if (error)
      printf("AFTER_CONNECT: receive buf4 err: %i\n", error);

    // Restore non-blocking mode
    so->so_state |= cur->nonblocking_flag;

    if (error == 0 && buf4[0] == 5 && buf4[1] == 1) {
      // Remote failure.
      // TODO: Maybe close the connection because the SOCKS proxy could not connect.
      printf("Remote failure: SOCKS proxy could not connect\n");
    }

    LIST_REMOVE(cur, ssod_list);

    return 1;
  }

  return 0;
}


int
shill_socket_check_create(struct ucred *cred,
                          int domain,
                          int type,
                          int protocol) {
  struct shill_session *session = shill_get_session(cred);
  if (session == NULL)
    return 0;

  if (session->ss_state == SESSION_DEBUG) {
    shilld_log((uintptr_t)session,
               "Permitting socket create for debugging");
    return 0;
  }

  if (type == SOCK_RAW) {
    shilld_log((uintptr_t)session, "No raw sockets permitted");
    return EACCES;
  }

  if (!shill_session_has_capability_to_create_in_af(session, domain, type)) {
    shilld_log((uintptr_t)session,
               "Insufficient capabilities for socket create in address "
               "family %d, with socket_type %d",
               domain,
               type);
    return EACCES;
  }
  return 0;
}

int
shill_socket_check_deliver(struct socket *so,
                           struct label *solabel,
                           struct mbuf *m,
                           struct label *mlabel) {
  // XXX what to do without ucred? How do I connect a socket capability with the
  // appropriate session (or even how do I know if they're in a session?)
  return 0;
}

int
shill_socket_check_listen(struct ucred *cred,
                          struct socket *so,
                          struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_listen",
                                 SHILL_SOCKET_CAP_LISTEN);
}

int
shill_socket_check_poll(struct ucred *cred,
                        struct socket *so,
                        struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_poll",
                                 SHILL_SOCKET_CAP_POLL);
}

int
shill_socket_check_receive(struct ucred *cred,
                           struct socket *so,
                           struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_receive",
                                 SHILL_SOCKET_CAP_RECV);
}

int
shill_socket_after_receive(struct ucred *cred,
                           struct socket *so,
                           struct msghdr *mp,
                           struct sockaddr *fromsa) {
  struct shill_session *session = shill_get_session(cred);
  if (session == NULL || !is_udp_proxy_enabled(session))
    return 0;

  if (fromsa != NULL && fromsa->sa_family == AF_INET && so->so_type == SOCK_DGRAM) {
    struct sockaddr_in *sa_in = (struct sockaddr_in *)fromsa;

    // Find original address information for proxying
    struct shill_socket_original_data_list *list = &session->ss_original_data_list;
    struct shill_socket_original_data *cur, *cur_temp;
    LIST_FOREACH_SAFE(cur, list, ssod_list, cur_temp) {
      if (cur->so == so) 	/* pointer equality */
        break;
    }
    if (cur == NULL)
      return 0;

    if (sa_in->sin_port == htons(session->dns_port) && sa_in->sin_addr.s_addr == session->dns_host->s_addr) {
      sa_in->sin_port = cur->port;
      sa_in->sin_addr.s_addr = cur->ip;
      LIST_REMOVE(cur, ssod_list);
      return 1;
    }
  }

  return 0;
}

int
shill_socket_check_send(struct ucred *cred,
                        struct socket *so,
                        struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_send",
                                 SHILL_SOCKET_CAP_SEND);
}

int
shill_socket_check_stat(struct ucred *cred,
                        struct socket *so,
                        struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_stat",
                                 SHILL_SOCKET_CAP_STAT);
}

int
shill_socket_check_visible(struct ucred *cred,
                           struct socket *so,
                           struct label *solabel) {
  return shill_network_cap_check(cred, so, "check_visible",
                                 SHILL_SOCKET_CAP_VISIBLE);
}

void
shill_socket_copy_label(struct label *src,
                        struct label *dest) {
  // XXX Implement this
}

void
shill_socket_create(struct ucred *cred,
                    struct socket *so,
                    struct label *solabel) {
  struct shill_session *session = shill_get_session(cred);
  if (session == NULL)
    return;

  struct shill_network_cap_list *session_cap_list =
    &session->ss_network_cap_list;

  struct shill_network_cap_list *socket_network_cap_list =
    (struct shill_network_cap_list *)mac_label_get(solabel, shill_slot);

  int address_family = shill_socket_get_af(so);

  struct shill_network_cap *session_cap;
  KASSERT(session_cap_list != NULL, ("shill_socket_create: null list"));
  LIST_FOREACH(session_cap, session_cap_list, snc_session_list) {
    if (session_cap->snc_af_family == address_family) {
      /* set up a new network cap for this socket */
      struct shill_network_cap *new_network_cap =
        shill_copy_network_cap(session_cap);

      /* put the new capability on this socket's capability list */
      LIST_INSERT_HEAD(socket_network_cap_list, new_network_cap, snc_socket_list);
    }
  }
}

void
shill_socket_create_mbuf(struct socket *so,
                         struct label *solabel,
                         struct mbuf *m,
                         struct label *mlabel) {
  // XXX Implement this
}

void
shill_socket_destroy_label(struct label *label) {
  struct shill_network_cap_list *network_cap_list =
    (struct shill_network_cap_list *)mac_label_get(label, shill_slot);
  shill_clear_network_cap_socket_list(network_cap_list);
  shill_network_cap_list_free(network_cap_list);
}

int
shill_socket_externalize_label(struct label *label,
                               char *element_name,
                               struct sbuf *sb,
                               int *claimed) {
  // XXX Implement this
  return 0;
}

void
shill_socket_newconn(struct socket *oldso,
                     struct label *oldsolabel,
                     struct socket *newso,
                     struct label *newsolabel) {
  struct shill_network_cap_list *old_socket_network_cap_list =
    (struct shill_network_cap_list *)mac_label_get(oldsolabel, shill_slot);
  struct shill_network_cap_list *new_socket_network_cap_list =
    (struct shill_network_cap_list *)mac_label_get(newsolabel, shill_slot);

  struct shill_network_cap *old_network_cap;
  KASSERT(session_cap_list != NULL, ("shill_socket_create: null list"));
  LIST_FOREACH(old_network_cap, old_socket_network_cap_list, snc_socket_list) {
    /* set up a new network cap for this socket */
    struct shill_network_cap *new_network_cap =
      shill_copy_network_cap(old_network_cap);

    /* put the new capability on this socket's capability list */
    LIST_INSERT_HEAD(new_socket_network_cap_list, new_network_cap, snc_socket_list);
  }
}


int
shill_socket_init_label(struct label *label, int flag) {
  /* allocate space for a list of network capabilities */
  struct shill_network_cap_list *list = shill_network_cap_list_alloc();
  LIST_INIT(list);

  mac_label_set(label, shill_slot, (uintptr_t)list);

  return 0;
}

int
shill_socket_internalize_label(struct label *label,
                               char *element_name,
                               char *element_data,
                               int *claimed) {
  // XXX implement this
  return 0;
}

/* int */
/* shill_socket_internalize_label(struct label *label, */
/*                                char *element_name, */
/*                                char *element_data, */
/*                                int *claimed) { */
/*   if (strcmp(MAC_SHILL_LABEL_NAME, element_name) != 0) */
/*     return 0; */

/*   (*claimed)++; */

/*   if (strncmp("socket/set/", element_data, 11) == 0) { */
/*     /\* NB that af_family and session are both uninitialized!!! *\/ */
/*     struct shill_network_cap *new_network_cap = */
/*       shill_network_cap_alloc(); */
/*     new_network_cap->snc_permissions = permissions_from_data(element_data + 11); */

/*     /\* insert the new_network_cap into the list *\/ */
/*     struct shill_network_cap_list *network_cap_list = */
/*       (struct shill_network_cap_list *)mac_label_get(label, shill_slot); */
/*     LIST_INSERT_HEAD(network_cap_list, new_network_cap, snc_socket_list); */
/*   } else { */
/*     return EINVAL; */
/*   } */

/*   return 0; */
/* } */

int
shill_socket_check_relabel(struct ucred *cred,
                           struct socket *so,
                           struct label *solabel,
                           struct label *newlabel) {
  struct shill_session *session = shill_get_session(cred);
  struct shill_network_cap_list *new_network_cap_list =
    shill_get_network_cap_list_from_socket_label(newlabel);
  struct shill_network_cap *new_cap;

  KASSERT(new_network_cap_list != NULL, ("shill_socket_check_relabel: null list"));
  LIST_FOREACH(new_cap, new_network_cap_list, snc_socket_list) {
    /*
     * NB: We may have more authority on a socket given to us than our session's
     * ambient authority. We may wish to relabel such a socket to a lower
     * authority. This is fine, even if our *session*'s ambient authority is weaker
     * than the proposed new authority.
     */
    if(new_cap->snc_session == session &&
       !shill_network_cap_lte_any_in_socket_for_session(new_cap, so, session) &&
       !shill_network_cap_lte_any_in_session(new_cap, session)) {
      return EACCES;
    }
  }

  return 0;
}

void
shill_socket_relabel(struct ucred *cred,
                     struct socket *so,
                     struct label *oldlabel,
                     struct label *newlabel) {
  struct shill_network_cap_list *new_network_cap_list =
    shill_get_network_cap_list_from_socket_label(newlabel);

  struct shill_network_cap_list *old_network_cap_list =
    shill_get_network_cap_list_from_socket_label(oldlabel);

  struct shill_network_cap *new_cap;
  KASSERT(new_network_cap_list != NULL, ("shill_socket_relabel: null list"));
  LIST_FOREACH(new_cap, new_network_cap_list, snc_socket_list) {
    LIST_INSERT_HEAD(old_network_cap_list, new_cap, snc_socket_list);
  }

  /*
   * NB: the old label's list is useless to us now; so let's free it
   */
  shill_network_cap_list_free(new_network_cap_list);
}
