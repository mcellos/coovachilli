/*
 * DHCP library functions.
 * Copyright (C) 2003, 2004, 2005, 2006 Mondru AB.
 * Copyright (c) 2006-2008 David Bird <david@coova.com>
 *
 * The contents of this file may be used under the terms of the GNU
 * General Public License Version 2, provided that the above copyright
 * notice and this permission notice is included in all copies or
 * substantial portions of the software.
 *
 */

#include "system.h"
#include "syserr.h"
#include "radius.h"
#include "radius_wispr.h"
#include "radius_chillispot.h"
#include "redir.h"
#include "md5.h"
#include "dhcp.h"
#include "dns.h"
#include "tun.h"
#include "chilli.h"
#include "options.h"
#include "ippool.h"
#include "lookup.h"

const uint32_t DHCP_OPTION_MAGIC      =  0x63825363;

#ifdef NAIVE
const static int paranoid = 0; /* Trust that the program has no bugs */
#else
const static int paranoid = 0; /* Check for errors which cannot happen */
#endif

extern time_t mainclock;

char *dhcp_state2name(int authstate) {
  switch(authstate) {
  case DHCP_AUTH_NONE: return "none";
  case DHCP_AUTH_DROP: return "drop";
  case DHCP_AUTH_PASS: return "pass";
  case DHCP_AUTH_DNAT: return "dnat";
  case DHCP_AUTH_SPLASH: return "splash";
  default: return "unknown";
  }
}

void dhcp_list(struct dhcp_t *this, bstring s, bstring pre, bstring post, int listfmt) {
  struct dhcp_conn_t *conn = this->firstusedconn;
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "{ \"sessions\":[");
  }
  while (conn) {
    if (pre) bconcat(s, pre);
    dhcp_print(this, s, listfmt, conn);
    if (post) bconcat(s, post);
    conn = conn->next;
  }
  if (listfmt == LIST_JSON_FMT) {
    bcatcstr(s, "]}");
  }
}

void dhcp_print(struct dhcp_t *this, bstring s, int listfmt, struct dhcp_conn_t *conn) {
  struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
  bstring b = bfromcstr("");
  bstring tmp = bfromcstr("");

  if (conn && conn->inuse) {

    if (listfmt == LIST_JSON_FMT) {

      if (conn != this->firstusedconn)
	bcatcstr(b, ",");

      bcatcstr(b, "{");

      if (appconn) {
	bcatcstr(b, "\"nasPort\":");
	bassignformat(tmp, "%d", appconn->unit);
	bconcat(b, tmp);
	bcatcstr(b, ",\"clientState\":");
	bassignformat(tmp, "%d", appconn->s_state.authenticated);
	bconcat(b, tmp);
	bcatcstr(b, ",\"macAddress\":\"");
	bassignformat(tmp, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",
		      conn->hismac[0], conn->hismac[1], conn->hismac[2],
		      conn->hismac[3], conn->hismac[4], conn->hismac[5]);
	bconcat(b, tmp);
	bcatcstr(b, "\",\"ipAddress\":\"");
	bcatcstr(b, inet_ntoa(conn->hisip));
	bcatcstr(b, "\"");
      }

    } else {

      bassignformat(b, "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X %s %s",
		    conn->hismac[0], conn->hismac[1], conn->hismac[2],
		    conn->hismac[3], conn->hismac[4], conn->hismac[5],
		    inet_ntoa(conn->hisip), dhcp_state2name(conn->authstate));

    }
    
    if (listfmt && this->cb_getinfo)
      this->cb_getinfo(conn, b, listfmt);

    if (listfmt == LIST_JSON_FMT)
      bcatcstr(b, "}");
    else
      bcatcstr(b, "\n");

    bconcat(s, b);
  }

  bdestroy(b);
  bdestroy(tmp);
}

void dhcp_release_mac(struct dhcp_t *this, uint8_t *hwaddr, int term_cause) {
  struct dhcp_conn_t *conn;
  if (!dhcp_hashget(this, &conn, hwaddr)) {
    if (conn->authstate == DHCP_AUTH_DROP &&
	term_cause != RADIUS_TERMINATE_CAUSE_ADMIN_RESET) 
      return;
    dhcp_freeconn(conn, term_cause);
  }
}

void dhcp_block_mac(struct dhcp_t *this, uint8_t *hwaddr) {
  struct dhcp_conn_t *conn;
  if (!dhcp_hashget(this, &conn, hwaddr)) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    conn->authstate = DHCP_AUTH_DROP;
    if (appconn) appconn->dnprot = DNPROT_NULL;
  }
}

int dhcp_send(struct dhcp_t *this, struct _net_interface *netif, 
	      unsigned char *hismac, void *packet, size_t length) {

  if (options.tcpwin) {
    struct tcp_fullheader_t *tcp_pack = (struct tcp_fullheader_t *)packet;
    if (tcp_pack->iph.protocol == PKT_IP_PROTO_TCP) {
      if (ntohs(tcp_pack->tcph.win) > options.tcpwin) {
	tcp_pack->tcph.win = htons(options.tcpwin);
	chksum(&tcp_pack->iph);
      }
    }
  }


#if defined(__linux__)
  struct sockaddr_ll dest;

  memset(&dest, 0, sizeof(dest));
  dest.sll_family = AF_PACKET;
  dest.sll_protocol = htons(netif->protocol);
  dest.sll_ifindex = netif->ifindex;
  if (hismac) {
    dest.sll_halen = PKT_ETH_ALEN;
    memcpy(dest.sll_addr, hismac, PKT_ETH_ALEN);
  }

  if (sendto(netif->fd, packet, length, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
#ifdef ENETDOWN
    if (errno == ENETDOWN) {
      net_reopen(netif);
    }
#endif
#ifdef ENXIO
    if (errno == ENXIO) {
      net_reopen(netif);
    }
#endif
    log_err(errno, "sendto(fd=%d, len=%d) failed", netif->fd, length);
    return -1;
  }
#elif defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__) || defined (__NetBSD__)
  if (write(netif->fd, packet, length) < 0) {
    log_err(errno, "write() failed");
    return -1;
  }
#endif
  return 0;
}


/**
 * dhcp_hash()
 * Generates a 32 bit hash based on a mac address
 **/
uint32_t dhcp_hash(uint8_t *hwaddr) {
  return lookup(hwaddr, PKT_ETH_ALEN, 0);
}


/**
 * dhcp_hashinit()
 * Initialises hash tables
 **/
int dhcp_hashinit(struct dhcp_t *this, int listsize) {
  /* Determine hashlog */
  for ((this)->hashlog = 0; 
       ((1 << (this)->hashlog) < listsize);
       (this)->hashlog++);
  
  /* Determine hashsize */
  (this)->hashsize = 1 << (this)->hashlog;
  (this)->hashmask = (this)->hashsize -1;
  
  /* Allocate hash table */
  if (!((this)->hash = calloc(sizeof(struct dhcp_conn_t), (this)->hashsize))){
    /* Failed to allocate memory for hash members */
    return -1;
  }
  return 0;
}


/**
 * dhcp_hashadd()
 * Adds a connection to the hash table
 **/
int dhcp_hashadd(struct dhcp_t *this, struct dhcp_conn_t *conn) {
  uint32_t hash;
  struct dhcp_conn_t *p;
  struct dhcp_conn_t *p_prev = NULL; 

  /* Insert into hash table */
  hash = dhcp_hash(conn->hismac) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash)
    p_prev = p;
  if (!p_prev)
    this->hash[hash] = conn;
  else 
    p_prev->nexthash = conn;
  return 0; /* Always OK to insert */
}


/**
 * dhcp_hashdel()
 * Removes a connection from the hash table
 **/
int dhcp_hashdel(struct dhcp_t *this, struct dhcp_conn_t *conn) {
  uint32_t hash;
  struct dhcp_conn_t *p;
  struct dhcp_conn_t *p_prev = NULL; 

  /* Find in hash table */
  hash = dhcp_hash(conn->hismac) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash) {
    if (p == conn) {
      break;
    }
    p_prev = p;
  }

  if ((paranoid) && (p!= conn)) {
    log_err(0, "Tried to delete connection not in hash table");
  }

  if (!p_prev)
    this->hash[hash] = p->nexthash;
  else
    p_prev->nexthash = p->nexthash;
  
  return 0;
}


/**
 * dhcp_hashget()
 * Uses the hash tables to find a connection based on the mac address.
 * Returns -1 if not found.
 **/
int dhcp_hashget(struct dhcp_t *this, struct dhcp_conn_t **conn,
		 uint8_t *hwaddr) {
  struct dhcp_conn_t *p;
  uint32_t hash;

  /* Find in hash table */
  hash = dhcp_hash(hwaddr) & this->hashmask;
  for (p = this->hash[hash]; p; p = p->nexthash) {
    if ((!memcmp(p->hismac, hwaddr, PKT_ETH_ALEN)) && (p->inuse)) {
      *conn = p;
      return 0;
    }
  }
  *conn = NULL;
  return -1; /* Address could not be found */
}


/**
 * dhcp_validate()
 * Valides reference structures of connections. 
 * Returns the number of active connections
 **/
int dhcp_validate(struct dhcp_t *this)
{
  int used = 0;
  int unused = 0;
  struct dhcp_conn_t *conn;
  struct dhcp_conn_t *hash_conn;

  /* Count the number of used connections */
  conn = this->firstusedconn;
  while (conn) {

    if (!conn->inuse) {
      log_err(0, "Connection with inuse == 0!");
    }
    
    dhcp_hashget(this, &hash_conn, conn->hismac);

    if (conn != hash_conn) {
      log_err(0, "Connection could not be found by hashget!");
    }

    used ++;
    conn = conn->next;
  }
  
  /* Count the number of unused connections */
  conn = this->firstfreeconn;
  while (conn) {
    if (conn->inuse) {
      log_err(0, "Connection with inuse != 0!");
    }
    unused ++;
    conn = conn->next;
  }

  if (this->numconn != (used + unused)) {
    log_err(0, "The number of free and unused connections does not match!");
    if (this->debug) {
      log_dbg("used %d unused %d", used, unused);
      conn = this->firstusedconn;
      while (conn) {
	log_dbg("%.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	       conn->hismac[0], conn->hismac[1], conn->hismac[2],
	       conn->hismac[3], conn->hismac[4], conn->hismac[5]);
	conn = conn->next;
      }
    }
  }
  
  return used;
}


/**
 * dhcp_initconn()
 * Initialises connection references
 **/
int dhcp_initconn(struct dhcp_t *this)
{
  int n;
  this->firstusedconn = NULL; /* Redundant */
  this->lastusedconn  = NULL; /* Redundant */

  for (n=0; n<this->numconn; n++) {
    this->conn[n].inuse = 0; /* Redundant */
    if (n == 0) {
      this->conn[n].prev = NULL; /* Redundant */
      this->firstfreeconn = &this->conn[n];

    }
    else {
      this->conn[n].prev = &this->conn[n-1];
      this->conn[n-1].next = &this->conn[n];
    }
    if (n == (this->numconn-1)) {
      this->conn[n].next = NULL; /* Redundant */
      this->lastfreeconn  = &this->conn[n];
    }
  }

  if (paranoid) dhcp_validate(this);

  return 0;
}

/**
 * dhcp_newconn()
 * Allocates a new connection from the pool. 
 * Returns -1 if unsuccessful.
 **/
int dhcp_newconn(struct dhcp_t *this, 
		 struct dhcp_conn_t **conn, 
		 uint8_t *hwaddr)
{

  if (this->debug) 
    log_dbg("DHCP newconn: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	    hwaddr[0], hwaddr[1], hwaddr[2],
	    hwaddr[3], hwaddr[4], hwaddr[5]);


  if (!this->firstfreeconn) {
    log_err(0, "Out of free connections");
    return -1;
  }

  *conn = this->firstfreeconn;

  /* Remove from link of free */
  if (this->firstfreeconn->next) {
    this->firstfreeconn->next->prev = NULL;
    this->firstfreeconn = this->firstfreeconn->next;
  }
  else { /* Took the last one */
    this->firstfreeconn = NULL; 
    this->lastfreeconn = NULL;
  }

  /* Initialise structures */
  memset(*conn, 0, sizeof(**conn));

  /* Insert into link of used */
  if (this->firstusedconn) {
    this->firstusedconn->prev = *conn;
    (*conn)->next = this->firstusedconn;
  }
  else { /* First insert */
    this->lastusedconn = *conn;
  }

  this->firstusedconn = *conn;

  (*conn)->inuse = 1;
  (*conn)->parent = this;
  (*conn)->mtu = this->mtu;
  (*conn)->noc2c = this->noc2c;

  /* Application specific initialisations */
  memcpy((*conn)->hismac, hwaddr, PKT_ETH_ALEN);
  memcpy((*conn)->ourmac, this->ipif.hwaddr, PKT_ETH_ALEN);
  (*conn)->lasttime = mainclock;
  
  dhcp_hashadd(this, *conn);
  
  if (paranoid) dhcp_validate(this);

  /* Inform application that connection was created */
  if (this->cb_connect)
    this->cb_connect(*conn);
  
  return 0; /* Success */
}


/**
 * dhcp_freeconn()
 * Returns a connection to the pool. 
 **/
int dhcp_freeconn(struct dhcp_conn_t *conn, int term_cause)
{
  /* TODO: Always returns success? */

  struct dhcp_t *this = conn->parent;

  /* Tell application that we disconnected */
  if (this->cb_disconnect)
    this->cb_disconnect(conn, term_cause);

  if (this->debug)
    log_dbg("DHCP freeconn: %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", 
	    conn->hismac[0], conn->hismac[1], conn->hismac[2],
	    conn->hismac[3], conn->hismac[4], conn->hismac[5]);


  /* Application specific code */
  /* First remove from hash table */
  dhcp_hashdel(this, conn);

  /* Remove from link of used */
  if ((conn->next) && (conn->prev)) {
    conn->next->prev = conn->prev;
    conn->prev->next = conn->next;
  }
  else if (conn->next) { /* && prev == 0 */
    conn->next->prev = NULL;
    this->firstusedconn = conn->next;
  }
  else if (conn->prev) { /* && next == 0 */
    conn->prev->next = NULL;
    this->lastusedconn = conn->prev;
  }
  else { /* if ((next == 0) && (prev == 0)) */
    this->firstusedconn = NULL;
    this->lastusedconn = NULL;
  }

  /* Initialise structures */
  memset(conn, 0, sizeof(*conn));

  /* Insert into link of free */
  if (this->firstfreeconn) {
    this->firstfreeconn->prev = conn;
  }
  else { /* First insert */
    this->lastfreeconn = conn;
  }

  conn->next = this->firstfreeconn;
  this->firstfreeconn = conn;

  if (paranoid) dhcp_validate(this);

  return 0;
}


/**
 * dhcp_checkconn()
 * Checks connections to see if the lease has expired
 **/
int dhcp_checkconn(struct dhcp_t *this)
{
  struct dhcp_conn_t *conn;
  time_t now = mainclock;

  now -= this->lease;
  conn = this->firstusedconn;
  while (conn) {
    if (now > conn->lasttime) {
      if (this->debug) 
	log_dbg("DHCP timeout: Removing connection");
      dhcp_freeconn(conn, RADIUS_TERMINATE_CAUSE_LOST_CARRIER);
      return 0; /* Returning after first deletion */
    }
    conn = conn->next;
  }
  return 0;
}

/**
 * dhcp_new()
 * Allocates a new instance of the library
 **/

int
dhcp_new(struct dhcp_t **pdhcp, int numconn, char *interface,
	 int usemac, uint8_t *mac, int promisc, 
	 struct in_addr *listen, int lease, int allowdyn,
	 struct in_addr *uamlisten, uint16_t uamport, int useeapol,
	 int noc2c) {
  struct dhcp_t *dhcp;
  
  if (!(dhcp = *pdhcp = calloc(sizeof(struct dhcp_t), 1))) {
    log_err(0, "calloc() failed");
    return -1;
  }

  dhcp->numconn = numconn;

  if (!(dhcp->conn = calloc(sizeof(struct dhcp_conn_t), numconn))) {
    log_err(0, "calloc() failed");
    free(dhcp);
    return -1;
  }

  dhcp_initconn(dhcp);

  if (net_init(&dhcp->ipif, interface, PKT_ETH_PROTO_IP, promisc, usemac ? mac : 0) < 0) {
    free(dhcp->conn);
    free(dhcp);
    return -1; 
  }

#if defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__)
  { 
    int blen=0;
    if (ioctl(dhcp->ipif.fd, BIOCGBLEN, &blen) < 0) {
      log_err(errno,"ioctl() failed!");
    }
    dhcp->rbuf_max = blen;
    if (!(dhcp->rbuf = calloc(dhcp->rbuf_max, 1))) {
      /* TODO: Free malloc */
      log_err(errno, "malloc() failed");
    }
    dhcp->rbuf_offset = 0;
    dhcp->rbuf_len = 0;
  }
#endif
  
  if (net_init(&dhcp->arpif, interface, PKT_ETH_PROTO_ARP, promisc, usemac ? mac : 0) < 0) {
      close(dhcp->ipif.fd);
      free(dhcp->conn);
      free(dhcp);
      return -1; /* Error reporting done in dhcp_open_eth */
    }

  if (useeapol) {
    if (net_init(&dhcp->eapif, interface, PKT_ETH_PROTO_EAPOL, promisc, usemac ? mac : 0) < 0) {
      close(dhcp->ipif.fd);
      close(dhcp->arpif.fd);
      free(dhcp->conn);
      free(dhcp);
      return -1; /* Error reporting done in eapol_open_eth */
    }
  }

  if (options.dhcpgwip.s_addr != 0) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;
    
    if (fd > 0) {

      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = dhcp->uamlisten.s_addr;

      /*
       * ====[http://tools.ietf.org/id/draft-ietf-dhc-implementation-02.txt]====
       * 4.7.2 Relay Agent Port Usage
       *    Relay agents should use port 67 as the source port number.  Relay
       *    agents always listen on port 67, but port 68 has sometimes been used
       *    as the source port number probably because it was copied from the
       *    source port of the incoming packet.
       * 
       *    Cable modem vendors would like to install filters blocking outgoing
       *    packets with source port 67.
       * 
       *    RECOMMENDATIONS:
       *      O  Relay agents MUST use 67 as their source port number.
       *      O  Relay agents MUST NOT forward packets with non-zero giaddr
       *         unless the source port number on the packet is 67.
       */

      addr.sin_port = htons(67);
      
      if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
	log_err(errno, "Can't set reuse option");
      }
      
      if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	log_err(errno, "socket or bind failed for dhcp relay!");
	close(fd);
	fd = -1;
      }
    }
      
    if (fd > 0) {
      dhcp->relayfd = fd;
    } else {
      close(dhcp->ipif.fd);
      close(dhcp->arpif.fd);
      close(dhcp->eapif.fd);
      free(dhcp->conn);
      free(dhcp);
      return -1;
    }
  }

  if (dhcp_hashinit(dhcp, dhcp->numconn))
    return -1; /* Failed to allocate hash tables */

  /* Initialise various variables */
  dhcp->ourip.s_addr = listen->s_addr;
  dhcp->lease = lease;
  dhcp->allowdyn = allowdyn;
  dhcp->uamlisten.s_addr = uamlisten->s_addr;
  dhcp->uamport = uamport;
  dhcp->mtu = options.mtu;
  dhcp->noc2c = noc2c;

  /* Initialise call back functions */
  dhcp->cb_data_ind = 0;
  dhcp->cb_eap_ind = 0;
  dhcp->cb_request = 0;
  dhcp->cb_disconnect = 0;
  dhcp->cb_connect = 0;
  
  return 0;
}

/**
 * dhcp_set()
 * Set dhcp parameters which can be altered at runtime.
 **/
int
dhcp_set(struct dhcp_t *dhcp, int debug) {
  dhcp->debug = debug;
  dhcp->anydns = options.uamanydns;

  /* Copy list of uamserver IP addresses */
  if (dhcp->authip) free(dhcp->authip);
  dhcp->authiplen = options.uamserverlen;

  if (!(dhcp->authip = calloc(sizeof(struct in_addr), options.uamserverlen))) {
    log_err(0, "calloc() failed");
    dhcp->authip = 0;
    return -1;
  }
  
  memcpy(dhcp->authip, &options.uamserver, sizeof(struct in_addr) * options.uamserverlen);

  return 0;
}

/**
 * dhcp_free()
 * Releases ressources allocated to the instance of the library
 **/
int dhcp_free(struct dhcp_t *dhcp) {
  if (dhcp->hash) free(dhcp->hash);
  if (dhcp->authip) free(dhcp->authip);
  dev_set_flags(dhcp->ipif.devname, dhcp->ipif.devflags);
  net_close(&dhcp->ipif);
  net_close(&dhcp->arpif);
  net_close(&dhcp->eapif);
  free(dhcp->conn);
  free(dhcp);
  return 0;
}

/**
 * dhcp_timeout()
 * Need to call this function at regular intervals to clean up old connections.
 **/
int
dhcp_timeout(struct dhcp_t *this)
{
  if (paranoid) 
    dhcp_validate(this);

  dhcp_checkconn(this);
  
  return 0;
}

/**
 * dhcp_timeleft()
 * Use this function to find out when to call dhcp_timeout()
 * If service is needed after the value given by tvp then tvp
 * is left unchanged.
 **/
struct timeval* dhcp_timeleft(struct dhcp_t *this, struct timeval *tvp) {
  return tvp;
}

int check_garden(pass_through *ptlist, int ptcnt, struct pkt_ippacket_t *pack, int dst) {
  struct pkt_tcphdr_t *tcph = (struct pkt_tcphdr_t *)pack->payload;
  struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;
  pass_through *pt;
  int i;

  for (i = 0; i < ptcnt; i++) {
    pt = &ptlist[i];
    if (pt->proto == 0 || pack->iph.protocol == pt->proto)
      if (pt->host.s_addr == 0 || 
	  pt->host.s_addr == ((dst ? pack->iph.daddr : pack->iph.saddr) & pt->mask.s_addr))
	if (pt->port == 0 || 
	    (pack->iph.protocol == PKT_IP_PROTO_TCP && (dst ? tcph->dst : tcph->src) == htons(pt->port)) ||
	    (pack->iph.protocol == PKT_IP_PROTO_UDP && (dst ? udph->dst : udph->src) == htons(pt->port)))
	  return 1;
  }

  return 0;
}

static
size_t tcprst(struct tcp_fullheader_t *tcp_pack, struct tcp_fullheader_t *orig_pack, char reverse) {

  size_t len = PKT_ETH_HLEN + PKT_IP_HLEN + PKT_TCP_HLEN;
  
  if (reverse) {

    memset(tcp_pack, 0, len);

    /* eth */
    memcpy(tcp_pack->ethh.dst, orig_pack->ethh.src, PKT_ETH_ALEN); 
    memcpy(tcp_pack->ethh.src, orig_pack->ethh.dst, PKT_ETH_ALEN); 
    tcp_pack->ethh.prot = orig_pack->ethh.prot;

    /* ip */
    memcpy(&tcp_pack->iph, &orig_pack->iph, PKT_IP_HLEN); 
    tcp_pack->iph.saddr = orig_pack->iph.daddr;
    tcp_pack->iph.daddr = orig_pack->iph.saddr;
    
    /* tcp */
    tcp_pack->tcph.src = orig_pack->tcph.dst;
    tcp_pack->tcph.dst = orig_pack->tcph.src;
    tcp_pack->tcph.seq = htonl(ntohl(orig_pack->tcph.seq)+1);

  } else {

    memcpy(tcp_pack, orig_pack, len); 

  }

  tcp_pack->iph.tot_len = htons(PKT_IP_HLEN + PKT_TCP_HLEN);

  tcp_pack->tcph.flags = 4;
  tcp_pack->tcph.offres = 0x50;

  chksum(&(tcp_pack->iph));

  return len;
}


static
void tun_sendRESET(struct tun_t *tun, void *pack, struct app_conn_t *appconn) {
  struct tcp_fullheader_t *orig_pack = (struct tcp_fullheader_t *)pack;
  struct tcp_fullheader_t tcp_pack;

  tun_encaps(tun, &tcp_pack, tcprst(&tcp_pack, orig_pack, 1), appconn->s_params.routeidx);
}

static
void dhcp_sendRESET(struct dhcp_conn_t *conn, void *pack, char reverse) {
  struct tcp_fullheader_t *orig_pack = (struct tcp_fullheader *)pack;
  struct tcp_fullheader_t tcp_pack;
  struct dhcp_t *this = conn->parent;
  
  dhcp_send(this, &this->ipif, conn->hismac, &tcp_pack, tcprst(&tcp_pack, orig_pack, reverse));
}

static
int dhcp_nakDNS(struct dhcp_conn_t *conn, struct pkt_ippacket_t *pack, size_t len) {
  struct dhcp_t *this = conn->parent;
  struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;
  /*struct dns_packet_t *dnsp = (struct dns_packet_t *)((char*)pack->payload + sizeof(struct pkt_udphdr_t));*/
  struct dns_fullpacket_t answer;

  memcpy(&answer, pack, len); 
    
  /* DNS response, with no host error code */
  answer.dns.flags = htons(0x8583); 
  
  /* UDP */
  answer.udph.src = udph->dst;
  answer.udph.dst = udph->src;
  
  /* IP */
  answer.iph.check = 0; /* Calculate at end of packet */      
  memcpy(&answer.iph.daddr, &pack->iph.saddr, PKT_IP_ALEN);
  memcpy(&answer.iph.saddr, &pack->iph.daddr, PKT_IP_ALEN);
  
  /* Ethernet */
  memcpy(&answer.ethh.dst, &pack->ethh.src, PKT_ETH_ALEN);
  memcpy(&answer.ethh.src, &pack->ethh.dst, PKT_ETH_ALEN);
  answer.ethh.prot = htons(PKT_ETH_PROTO_IP);
    
  /* checksums */
  chksum(&answer.iph);
  
  dhcp_send(this, &this->ipif, conn->hismac, &answer, len);

  return 0;
}

static 
int _filterDNSreq(struct dhcp_conn_t *conn, struct pkt_ippacket_t *pack, size_t plen) {
  /*struct dhcp_udphdr_t *udph = (struct dhcp_udphdr_t*)pack->payload;*/
  struct dns_packet_t *dnsp = (struct dns_packet_t *)((char*)pack->payload + sizeof(struct pkt_udphdr_t));
  size_t len = plen - DHCP_DNS_HLEN - PKT_UDP_HLEN - PKT_IP_HLEN - PKT_ETH_HLEN;
  size_t olen = len;

  uint16_t id = ntohs(dnsp->id);
  uint16_t flags = ntohs(dnsp->flags);
  uint16_t qdcount = ntohs(dnsp->qdcount);
  uint16_t ancount = ntohs(dnsp->ancount);
  uint16_t nscount = ntohs(dnsp->nscount);
  uint16_t arcount = ntohs(dnsp->arcount);

  uint8_t *p_pkt = (uint8_t *)dnsp->records;
  char q[256];

  int d = options.debug; /* XXX: debug */
  int i;

  if (d) log_dbg("DNS ID:    %d", id);
  if (d) log_dbg("DNS Flags: %d", flags);

  /* it was a response? shouldn't be */
  /*if (((flags & 0x8000) >> 15) == 1) return 0;*/

  memset(q,0,sizeof(q));

#undef  copyres
#define copyres(isq,n)			        \
  if (d) log_dbg(#n ": %d", n ## count);        \
  for (i=0; i < n ## count; i++)                \
    if (dns_copy_res(isq, &p_pkt, &len,         \
		     (uint8_t *)dnsp, olen, 	\
                     q, sizeof(q)))	        \
      return dhcp_nakDNS(conn,pack,plen)

  copyres(1,qd);
  copyres(0,an);
  copyres(0,ns);
  copyres(0,ar);

  if (d) log_dbg("left (should be zero): %d", len);

  return 1;
}

static
int _filterDNSresp(struct dhcp_conn_t *conn, struct pkt_ippacket_t *pack, size_t plen) {
  /*struct dhcp_udphdr_t *udph = (struct dhcp_udphdr_t*)pack->payload;*/
  struct dns_packet_t *dnsp = (struct dns_packet_t *)((char*)pack->payload + sizeof(struct pkt_udphdr_t));
  size_t len = plen - DHCP_DNS_HLEN - PKT_UDP_HLEN - PKT_IP_HLEN - PKT_ETH_HLEN;
  size_t olen = len;

  uint16_t id = ntohs(dnsp->id);
  uint16_t flags = ntohs(dnsp->flags);
  uint16_t qdcount = ntohs(dnsp->qdcount);
  uint16_t ancount = ntohs(dnsp->ancount);
  uint16_t nscount = ntohs(dnsp->nscount);
  uint16_t arcount = ntohs(dnsp->arcount);

  uint8_t *p_pkt = (uint8_t *)dnsp->records;
  char q[256];

  int d = options.debug; /* XXX: debug */
  int i;

  if (d) log_dbg("DNS ID:    %d", id);
  if (d) log_dbg("DNS Flags: %d", flags);

  /* it was a query? shouldn't be */
  if (((flags & 0x8000) >> 15) == 0) return 0;

  memset(q,0,sizeof(q));

#undef  copyres
#define copyres(isq,n)			        \
  if (d) log_dbg(#n ": %d", n ## count);        \
  for (i=0; i < n ## count; i++)                \
    dns_copy_res(isq, &p_pkt, &len,             \
		     (uint8_t *)dnsp, olen, 	\
                     q, sizeof(q))

  copyres(1,qd);
  copyres(0,an);
  copyres(0,ns);
  copyres(0,ar);

  if (d) log_dbg("left (should be zero): %d", len);

  /*
  dnsp->flags = htons(flags);
  dnsp->qdcount = htons(qdcount);
  dnsp->ancount = htons(ancount);
  dnsp->nscount = htons(nscount);
  dnsp->arcount = htons(arcount);
  */

  return 1;
}


/**
 * dhcp_doDNAT()
 * Change destination address to authentication server.
 **/
int dhcp_doDNAT(struct dhcp_conn_t *conn, 
		struct pkt_ippacket_t *pack, size_t len) {
  struct dhcp_t *this = conn->parent;
  struct pkt_tcphdr_t *tcph = (struct pkt_tcphdr_t *)pack->payload;
  struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;
  int i;

  /* Allow localhost through network... */
  if (pack->iph.daddr == INADDR_LOOPBACK)
    return 0;

  /* Was it an ICMP request for us? */
  if (pack->iph.protocol == PKT_IP_PROTO_ICMP)
    if (pack->iph.daddr == conn->ourip.s_addr)
      return 0;

  /* Was it a DNS request? */
  if (((this->anydns) ||
       (pack->iph.daddr == conn->dns1.s_addr) ||
       (pack->iph.daddr == conn->dns2.s_addr)) &&
      (pack->iph.protocol == PKT_IP_PROTO_UDP && udph->dst == htons(DHCP_DNS))) {
    if (options.dnsparanoia) {
      if (_filterDNSreq(conn, pack, len)) 
	return 0;
      else /* drop */
	return -1;
    } else { /* allow */
      return 0;
    }
  }

  /* Was it a request for authentication server? */
  for (i = 0; i<this->authiplen; i++) {
    if ((pack->iph.daddr == this->authip[i].s_addr) /* &&
	(pack->iph.protocol == PKT_IP_PROTO_TCP) &&
	((tcph->dst == htons(DHCP_HTTP)) ||
	(tcph->dst == htons(DHCP_HTTPS)))*/)
      return 0; /* Destination was authentication server */
  }

  /* Was it a request for local redirection server? */
  if ((pack->iph.daddr == this->uamlisten.s_addr) &&
      (pack->iph.protocol == PKT_IP_PROTO_TCP) &&
      (tcph->dst == htons(this->uamport)))
    return 0; /* Destination was local redir server */

  /* Was it a request for a pass-through entry? */
  if (check_garden(options.pass_throughs, options.num_pass_throughs, pack, 1))
    return 0;
  /* Check uamdomain driven walled garden */
  if (check_garden(this->pass_throughs, this->num_pass_throughs, pack, 1))
    return 0;

  /* Check appconn session specific pass-throughs */
  if (conn->peer) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (check_garden(appconn->s_params.pass_throughs, appconn->s_params.pass_through_count, pack, 1))
      return 0;
  }

  if (pack->iph.protocol == PKT_IP_PROTO_TCP) {

    if (tcph->dst == htons(DHCP_HTTP)) {
      /* Was it a http request for another server? */
      /* We are changing dest IP and dest port to local UAM server */
      int n;
      int pos=-1;
      
      for (n=0; n<DHCP_DNAT_MAX; n++) {
	if ((conn->dnatip[n] == pack->iph.daddr) && 
	    (conn->dnatport[n] == tcph->src)) {
	  pos = n;
	  break;
	}
      }
      if (pos==-1) { /* Save for undoing */
	if (options.usetap) 
	  memcpy(conn->dnatmac[conn->nextdnat], pack->ethh.dst, PKT_ETH_ALEN); 
	conn->dnatip[conn->nextdnat] = pack->iph.daddr; 
	conn->dnatport[conn->nextdnat] = tcph->src;
	conn->nextdnat = (conn->nextdnat + 1) % DHCP_DNAT_MAX;
      }
      
      if (options.usetap) 
      memcpy(pack->ethh.dst, tuntap(tun).hwaddr, PKT_ETH_ALEN); 
      
      pack->iph.daddr = this->uamlisten.s_addr;
      tcph->dst = htons(this->uamport);
      
      chksum(&pack->iph);

      return 0;

    } else {

      /* otherwise, RESET and drop */

      dhcp_sendRESET(conn, pack, 1);
    }
  }
  
  return -1; /* Something else */

}

int dhcp_postauthDNAT(struct dhcp_conn_t *conn, struct pkt_ippacket_t *pack, size_t len, int isreturn) {
  struct dhcp_t *this = conn->parent;
  struct pkt_tcphdr_t *tcph = (struct pkt_tcphdr_t *)pack->payload;
  /*struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;*/

  if (options.postauth_proxyport > 0) {
    if (isreturn) {
      if ((pack->iph.protocol == PKT_IP_PROTO_TCP) &&
	  (pack->iph.saddr == options.postauth_proxyip.s_addr) &&
	  (tcph->src == htons(options.postauth_proxyport))) {
	int n;
	for (n=0; n<DHCP_DNAT_MAX; n++) {
	  if (tcph->dst == conn->dnatport[n]) {
	    if (options.usetap) 
	      memcpy(pack->ethh.src, conn->dnatmac[n], PKT_ETH_ALEN); 
	    pack->iph.saddr = conn->dnatip[n];
	    tcph->src = htons(DHCP_HTTP);

	    chksum(&pack->iph);

	    return 0; /* It was a DNAT reply */
	  }
	}
	return 0; 
      }
    }
    else {
      if ((pack->iph.protocol == PKT_IP_PROTO_TCP) &&
	  (tcph->dst == htons(DHCP_HTTP))) {

	int n;
	int pos=-1;

	for (n = 0; n<this->authiplen; n++)
	  if ((pack->iph.daddr == this->authip[n].s_addr))
	      return 0;
	
	for (n=0; n<DHCP_DNAT_MAX; n++) {
	  if ((conn->dnatip[n] == pack->iph.daddr) && 
	      (conn->dnatport[n] == tcph->src)) {
	    pos = n;
	    break;
	  }
	}
	
	if (pos==-1) { /* Save for undoing */
	  if (options.usetap) 
	    memcpy(conn->dnatmac[conn->nextdnat], pack->ethh.dst, PKT_ETH_ALEN); 
	  conn->dnatip[conn->nextdnat] = pack->iph.daddr; 
	  conn->dnatport[conn->nextdnat] = tcph->src;
	  conn->nextdnat = (conn->nextdnat + 1) % DHCP_DNAT_MAX;
	}
	
	log_dbg("rewriting packet for post-auth proxy %s:%d",
		inet_ntoa(options.postauth_proxyip),
		options.postauth_proxyport);
	
	pack->iph.daddr = options.postauth_proxyip.s_addr;
	tcph->dst = htons(options.postauth_proxyport);

	chksum(&pack->iph);

	return 0;
      }
    }
  }

  return -1; /* Something else */
}

/**
 * dhcp_undoDNAT()
 * Change source address back to original server
 **/
int dhcp_undoDNAT(struct dhcp_conn_t *conn, struct pkt_ippacket_t *pack, size_t *plen) {
  struct dhcp_t *this = conn->parent;
  struct pkt_tcphdr_t *tcph = (struct pkt_tcphdr_t *)pack->payload;
  struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;
  /*size_t len = *plen;*/
  int i;

  /* Allow localhost through network... */
  if (pack->iph.saddr == INADDR_LOOPBACK)
    return 0;

  /* Was it a DNS reply? */
  if (((this->anydns) ||
       (pack->iph.saddr == conn->dns1.s_addr) ||
       (pack->iph.saddr == conn->dns2.s_addr)) &&
      (pack->iph.protocol == PKT_IP_PROTO_UDP && udph->src == htons(DHCP_DNS))) {
    if (options.uamdomains) {
	if (_filterDNSresp(conn, pack, *plen)) 
	  return 0;
	else
	  return -1; /* drop */
    } else {   /* always let through dns when not filtering */
      return 0;
    }
  }

  if (pack->iph.protocol == PKT_IP_PROTO_ICMP) {
    /* Was it an ICMP reply from us? */
    if (pack->iph.saddr == conn->ourip.s_addr)
      return 0;
    /* Allow for MTU negotiation */
    if (options.debug)
      log_dbg("Received ICMP type=%d code=%d",
	      (int)pack->payload[0],(int)pack->payload[1]);
    switch((unsigned char)pack->payload[0]) {
    case 0:  /* echo reply */
    case 3:  /* destination unreachable */
    case 5:  /* redirect */
    case 11: /* time excedded */
      switch((unsigned char)pack->payload[1]) {
      case 4: 
	log(LOG_NOTICE, "Fragmentation needed ICMP");
      }
      if (options.debug)
	log_dbg("Forwarding ICMP to chilli client");
      return 0;
    }
    /* fail all else */
    return -1;
  }

  /*
12:46:20.767600 IP 10.1.0.1.49335 > 68.142.197.198.80: S 1442428713:1442428713(0) win 65535 <mss 1460,sackOK,eol>
12:46:20.768234 IP 10.1.0.10.3990 > 10.1.0.1.49335: S 746818639:746818639(0) ack 1442428714 win 5840 <mss 1460,nop,nop,sackOK>
  */

  /* Was it a reply from redir server? */
  if ((pack->iph.saddr == this->uamlisten.s_addr) &&
      (pack->iph.protocol == PKT_IP_PROTO_TCP) &&
      (tcph->src == htons(this->uamport))) {
    int n;

    for (n=0; n<DHCP_DNAT_MAX; n++) {
      if (tcph->dst == conn->dnatport[n]) {

	if (options.usetap) 
	  memcpy(pack->ethh.src, conn->dnatmac[n], PKT_ETH_ALEN); 

	pack->iph.saddr = conn->dnatip[n];
	tcph->src = htons(DHCP_HTTP);

	chksum(&pack->iph);

	return 0; /* It was a DNAT reply */
      }
    }
    return 0; /* It was a normal reply from redir server */
  }
  
  /* Was it a normal http or https reply from authentication server? */
  /* Was it a normal reply from authentication server? */
  for (i = 0; i<this->authiplen; i++) {
    if ((pack->iph.saddr == this->authip[i].s_addr) /* &&
	(pack->iph.protocol == PKT_IP_PROTO_TCP) &&
	((tcph->src == htons(DHCP_HTTP)) ||
	(tcph->src == htons(DHCP_HTTPS)))*/)
      return 0; /* Destination was authentication server */
  }
  
  /* Was it a reply for a pass-through entry? */
  if (check_garden(options.pass_throughs, options.num_pass_throughs, pack, 0))
    return 0;
  if (check_garden(this->pass_throughs, this->num_pass_throughs, pack, 0))
    return 0;

  /* Check appconn session specific pass-throughs */
  if (conn->peer) {
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (check_garden(appconn->s_params.pass_throughs, appconn->s_params.pass_through_count, pack, 0))
      return 0;
  }

  if (pack->iph.protocol == PKT_IP_PROTO_TCP) {
    dhcp_sendRESET(conn, pack, 0);
    if (conn->peer) {
      tun_sendRESET(tun, pack, (struct app_conn_t *)conn->peer);
    }
  }

  return -1; /* Something else */
}

/**
 * dhcp_checkDNS()
 * Check if it was request for known domain name.
 * In case it was a request for a known keyword then
 * redirect to the login/logout page
 * 2005-09-19: This stuff is highly experimental.
 **/
int dhcp_checkDNS(struct dhcp_conn_t *conn, 
		  struct pkt_ippacket_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct pkt_udphdr_t *udph = (struct pkt_udphdr_t *)pack->payload;
  struct dns_packet_t *dnsp = (struct dns_packet_t *)((char*)pack->payload + sizeof(struct pkt_udphdr_t));
  struct dns_fullpacket_t answer;
  uint8_t *p1 = NULL;
  uint8_t *p2 = NULL;
  size_t length;
  size_t udp_len;
  uint8_t query[256];
  size_t query_len = 0;
  int n;

  log_dbg("DNS ID:    %d", ntohs(dnsp->id));
  log_dbg("DNS flags: %d", ntohs(dnsp->flags));

  if ((ntohs(dnsp->flags)   == 0x0100) &&
      (ntohs(dnsp->qdcount) == 0x0001) &&
      (ntohs(dnsp->ancount) == 0x0000) &&
      (ntohs(dnsp->nscount) == 0x0000) &&
      (ntohs(dnsp->arcount) == 0x0000)) {

    log_dbg("It was a query %s", dnsp->records);

    p1 = dnsp->records + 1 + dnsp->records[0];
    p2 = dnsp->records;

    do {
      if (query_len < 256)
	query[query_len++] = *p2;
    } while (*p2++ != 0); /* TODO */

    for (n=0; n<4; n++) {
      if (query_len < 256)
	query[query_len++] = *p2++;
    }

    query[query_len++] = 0xc0;
    query[query_len++] = 0x0c;
    query[query_len++] = 0x00;
    query[query_len++] = 0x01;
    query[query_len++] = 0x00;
    query[query_len++] = 0x01;
    query[query_len++] = 0x00;
    query[query_len++] = 0x00;
    query[query_len++] = 0x01;
    query[query_len++] = 0x2c;
    query[query_len++] = 0x00;
    query[query_len++] = 0x04;
    memcpy(&query[query_len], &conn->ourip.s_addr, 4);
    query_len += 4;

    if (!memcmp(p1, 
		"\3key\12chillispot\3org", 
		sizeof("\3key\12chillispot\3org"))) {
      log_dbg("It was a matching query %s: \n", dnsp->records);
      memcpy(&answer, pack, len); /* TODO */
      
      /* DNS Header */
      answer.dns.id      = dnsp->id;
      answer.dns.flags   = htons(0x8000);
      answer.dns.qdcount = htons(0x0001);
      answer.dns.ancount = htons(0x0001);
      answer.dns.nscount = htons(0x0000);
      answer.dns.arcount = htons(0x0000);
      memcpy(answer.dns.records, query, query_len);
      
      /* UDP header */
      udp_len = query_len + DHCP_DNS_HLEN + PKT_UDP_HLEN;
      answer.udph.len = htons(udp_len);
      answer.udph.src = udph->dst;
      answer.udph.dst = udph->src;
      
      /* IP header */
      answer.iph.version_ihl = PKT_IP_VER_HLEN;
      answer.iph.tos = 0;
      answer.iph.tot_len = htons(udp_len + PKT_IP_HLEN);
      answer.iph.id = 0;
      answer.iph.frag_off = 0;
      answer.iph.ttl = 0x10;
      answer.iph.protocol = 0x11;
      answer.iph.check = 0; /* Calculate at end of packet */      
      memcpy(&answer.iph.daddr, &pack->iph.saddr, PKT_IP_ALEN);
      memcpy(&answer.iph.saddr, &pack->iph.saddr, PKT_IP_ALEN);

      /* Ethernet header */
      memcpy(&answer.ethh.dst, &pack->ethh.src, PKT_ETH_ALEN);
      memcpy(&answer.ethh.src, &pack->ethh.dst, PKT_ETH_ALEN);
      answer.ethh.prot = htons(PKT_ETH_PROTO_IP);

      /* Work out checksums */
      chksum(&answer.iph);

      /* Calculate total length */
      length = udp_len + PKT_IP_HLEN + PKT_ETH_HLEN;
      
      return dhcp_send(this, &this->ipif, conn->hismac, &answer, length);
    }
  }
  return -1; /* Something else */
}

/**
 * dhcp_getdefault()
 * Fill in a DHCP packet with most essential values
 **/
int
dhcp_getdefault(struct dhcp_fullpacket_t *pack) {

  /* Initialise reply packet with request */
  memset(pack, 0, sizeof(struct dhcp_fullpacket_t));

  /* DHCP Payload */
  pack->dhcp.op     = DHCP_BOOTREPLY;
  pack->dhcp.htype  = DHCP_HTYPE_ETH;
  pack->dhcp.hlen   = PKT_ETH_ALEN;

  /* IP header */
  pack->iph.version_ihl = PKT_IP_VER_HLEN;
  pack->iph.tos = 0;
  pack->iph.tot_len = 0; /* Calculate at end of packet */
  pack->iph.id = 0;
  pack->iph.frag_off = 0;
  pack->iph.ttl = 0x10;
  pack->iph.protocol = 0x11;
  pack->iph.check = 0; /* Calculate at end of packet */

  /* Ethernet header */
  pack->ethh.prot = htons(PKT_ETH_PROTO_IP);

  return 0;
}

/**
 * dhcp_create_pkt()
 * Create a new typed DHCP packet
 */
int
dhcp_create_pkt(uint8_t type, struct dhcp_fullpacket_t *pack, 
		struct dhcp_fullpacket_t *req, struct dhcp_conn_t *conn) {
  struct dhcp_t *this = conn->parent;
  int pos = 0;

  dhcp_getdefault(pack);

  pack->dhcp.xid      = req->dhcp.xid;
  pack->dhcp.flags[0] = req->dhcp.flags[0];
  pack->dhcp.flags[1] = req->dhcp.flags[1];
  pack->dhcp.giaddr   = req->dhcp.giaddr;

  memcpy(&pack->dhcp.chaddr, &req->dhcp.chaddr, DHCP_CHADDR_LEN);
  memcpy(&pack->dhcp.sname, conn->dhcp_opts.sname, DHCP_SNAME_LEN);
  memcpy(&pack->dhcp.file, conn->dhcp_opts.file, DHCP_FILE_LEN);

  log_dbg("!!! dhcp server : %s !!!", pack->dhcp.sname);

  switch(type) {
  case DHCPOFFER:
    pack->dhcp.yiaddr = conn->hisip.s_addr;
    break;
  case DHCPACK:
    pack->dhcp.xid    = req->dhcp.xid;
    pack->dhcp.ciaddr = req->dhcp.ciaddr;
    pack->dhcp.yiaddr = conn->hisip.s_addr;
    break;
  case DHCPNAK:
    break;
  }

  /* Ethernet Header */
  memcpy(pack->ethh.dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(pack->ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);

  /* UDP and IP Headers */
  pack->udph.src = htons(DHCP_BOOTPS);
  pack->iph.saddr = conn->ourip.s_addr;

  /** http://www.faqs.org/rfcs/rfc1542.html
      BOOTREQUEST fields     BOOTREPLY values for UDP, IP, link-layer
   +-----------------------+-----------------------------------------+
   | 'ciaddr'  'giaddr'  B | UDP dest     IP destination   link dest |
   +-----------------------+-----------------------------------------+
   | non-zero     X      X | BOOTPC (68)  'ciaddr'         normal    |
   | 0.0.0.0   non-zero  X | BOOTPS (67)  'giaddr'         normal    |
   | 0.0.0.0   0.0.0.0   0 | BOOTPC (68)  'yiaddr'         'chaddr'  |
   | 0.0.0.0   0.0.0.0   1 | BOOTPC (68)  255.255.255.255  broadcast |
   +-----------------------+-----------------------------------------+

        B = BROADCAST flag

        X = Don't care

   normal = determine from the given IP destination using normal
            IP routing mechanisms and/or ARP as for any other
            normal datagram

   If the 'giaddr' field in a DHCP message from a client is non-zero,
   the server sends any return messages to the 'DHCP server' port on the
   BOOTP relay agent whose address appears in 'giaddr'. 

   If the 'giaddr' field is zero and the 'ciaddr' field is nonzero, then the
   server unicasts DHCPOFFER and DHCPACK messages to the address in
   'ciaddr'.  

   If 'giaddr' is zero and 'ciaddr' is zero, and the broadcast bit is set,
   then the server broadcasts DHCPOFFER and DHCPACK messages to
   0xffffffff. 

   If the broadcast bit is not set and 'giaddr' is zero and 'ciaddr' is
   zero, then the server unicasts DHCPOFFER and DHCPACK messages to the
   client's hardware address and 'yiaddr' address.  

   In all cases, when 'giaddr' is zero, the server broadcasts any DHCPNAK
   messages to 0xffffffff.

  **/

  if (req->dhcp.ciaddr) {
    pack->iph.daddr = req->dhcp.ciaddr; 
    pack->udph.dst = htons(DHCP_BOOTPC);
  } else if (req->dhcp.giaddr) {
    pack->iph.daddr = req->dhcp.giaddr; 
    pack->udph.dst = htons(DHCP_BOOTPS);
  } else if (type == DHCPNAK || req->dhcp.flags[0] & 0x80) {
    pack->iph.daddr = ~0; 
    pack->udph.dst = htons(DHCP_BOOTPC);
    pack->dhcp.flags[0] = 0x80;
  } else {
    pack->iph.daddr = pack->dhcp.yiaddr; 
    pack->udph.dst = htons(DHCP_BOOTPC);
  }

  /* Magic cookie */
  pack->dhcp.options[pos++] = 0x63;
  pack->dhcp.options[pos++] = 0x82;
  pack->dhcp.options[pos++] = 0x53;
  pack->dhcp.options[pos++] = 0x63;

  pack->dhcp.options[pos++] = DHCP_OPTION_MESSAGE_TYPE;
  pack->dhcp.options[pos++] = 1;
  pack->dhcp.options[pos++] = type;

  memcpy(&pack->dhcp.options[pos], conn->dhcp_opts.options, DHCP_OPTIONS_LEN-pos);
  pos += conn->dhcp_opts.option_length;

  return pos;
}


/**
 * dhcp_gettag()
 * Search a DHCP packet for a particular tag.
 * Returns -1 if not found.
 **/
int dhcp_gettag(struct dhcp_packet_t *pack, size_t length,
		struct dhcp_tag_t **tag, uint8_t tagtype) {
  struct dhcp_tag_t *t;
  size_t offset = DHCP_MIN_LEN + DHCP_OPTION_MAGIC_LEN;

  /* if (length > DHCP_LEN) {
    log_warn(0,"Length of dhcp packet larger then %d: %d", DHCP_LEN, length);
    length = DHCP_LEN;
  } */
  
  while ((offset + 2) < length) {
    t = (struct dhcp_tag_t *)(((void *)pack) + offset);
    if (t->t == tagtype) {
      if ((offset +  2 + (size_t)(t->l)) > length)
	return -1; /* Tag length too long */
      *tag = t;
      return 0;
    }
    offset +=  2 + t->l;
  }
  
  return -1; /* Not found  */
}


/**
 * dhcp_sendOFFER()
 * Send of a DHCP offer message to a peer.
 **/
int dhcp_sendOFFER(struct dhcp_conn_t *conn, 
		   struct dhcp_fullpacket_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct dhcp_fullpacket_t packet;
  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  pos = dhcp_create_pkt(DHCPOFFER, &packet, pack, conn);
  
  /* DHCP Payload */

  packet.dhcp.options[pos++] = DHCP_OPTION_SUBNET_MASK;
  packet.dhcp.options[pos++] = 4;
  if (conn->noc2c)
    memset(&packet.dhcp.options[pos], 0xff, 4);
  else
    memcpy(&packet.dhcp.options[pos], &conn->hismask.s_addr, 4);
  pos += 4;

  if (conn->noc2c) {
    packet.dhcp.options[pos++] = DHCP_OPTION_STATIC_ROUTES;
    packet.dhcp.options[pos++] = 8;
    memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
    pos += 4;
    memcpy(&packet.dhcp.options[pos], &conn->hisip.s_addr, 4);
    pos += 4;
  }

  packet.dhcp.options[pos++] = DHCP_OPTION_ROUTER_OPTION;
  packet.dhcp.options[pos++] = 4;
  memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  /* Insert DNS Servers if given */
  if (conn->dns1.s_addr && conn->dns2.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 8;
    memcpy(&packet.dhcp.options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns1.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns2.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }

  /* Insert Domain Name if present */
  if (strlen(conn->domain)) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DOMAIN_NAME;
    packet.dhcp.options[pos++] = strlen(conn->domain);
    memcpy(&packet.dhcp.options[pos], &conn->domain, strlen(conn->domain));
    pos += strlen(conn->domain);
  }

  packet.dhcp.options[pos++] = DHCP_OPTION_LEASE_TIME;
  packet.dhcp.options[pos++] = 4;
  packet.dhcp.options[pos++] = (this->lease >> 24) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >> 16) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >>  8) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >>  0) & 0xFF;

  /* Must be listening address */
  packet.dhcp.options[pos++] = DHCP_OPTION_SERVER_ID;
  packet.dhcp.options[pos++] = 4;
  memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet.dhcp.options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet.udph.len = htons(udp_len);

  /* IP header */
  packet.iph.tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(&packet.iph);

  /* Calculate total length */
  length = udp_len + PKT_IP_HLEN + PKT_ETH_HLEN;

  return dhcp_send(this, &this->ipif, conn->hismac, &packet, length);
}

/**
 * dhcp_sendACK()
 * Send of a DHCP acknowledge message to a peer.
 **/
int dhcp_sendACK(struct dhcp_conn_t *conn, 
		 struct dhcp_fullpacket_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct dhcp_fullpacket_t packet;
  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  pos = dhcp_create_pkt(DHCPACK, &packet, pack, conn);
  
  /* DHCP Payload */
  packet.dhcp.options[pos++] = DHCP_OPTION_SUBNET_MASK;
  packet.dhcp.options[pos++] = 4;
  if (conn->noc2c)
    memset(&packet.dhcp.options[pos], 0xff, 4);
  else
    memcpy(&packet.dhcp.options[pos], &conn->hismask.s_addr, 4);
  pos += 4;

  if (conn->noc2c) {
    packet.dhcp.options[pos++] = DHCP_OPTION_STATIC_ROUTES;
    packet.dhcp.options[pos++] = 8;
    memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
    pos += 4;
    memcpy(&packet.dhcp.options[pos], &conn->hisip.s_addr, 4);
    pos += 4;
  }

  packet.dhcp.options[pos++] = DHCP_OPTION_ROUTER_OPTION;
  packet.dhcp.options[pos++] = 4;
  memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  /* Insert DNS Servers if given */
  if (conn->dns1.s_addr && conn->dns2.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 8;
    memcpy(&packet.dhcp.options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns1.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns1.s_addr, 4);
    pos += 4;
  }
  else if (conn->dns2.s_addr) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DNS;
    packet.dhcp.options[pos++] = 4;
    memcpy(&packet.dhcp.options[pos], &conn->dns2.s_addr, 4);
    pos += 4;
  }

  /* Insert Domain Name if present */
  if (strlen(conn->domain)) {
    packet.dhcp.options[pos++] = DHCP_OPTION_DOMAIN_NAME;
    packet.dhcp.options[pos++] = strlen(conn->domain);
    memcpy(&packet.dhcp.options[pos], &conn->domain, strlen(conn->domain));
    pos += strlen(conn->domain);
  }

  packet.dhcp.options[pos++] = DHCP_OPTION_LEASE_TIME;
  packet.dhcp.options[pos++] = 4;
  packet.dhcp.options[pos++] = (this->lease >> 24) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >> 16) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >>  8) & 0xFF;
  packet.dhcp.options[pos++] = (this->lease >>  0) & 0xFF;

  packet.dhcp.options[pos++] = DHCP_OPTION_INTERFACE_MTU;
  packet.dhcp.options[pos++] = 2;
  packet.dhcp.options[pos++] = (conn->mtu >> 8) & 0xFF;
  packet.dhcp.options[pos++] = (conn->mtu >> 0) & 0xFF;

  /* Must be listening address */
  packet.dhcp.options[pos++] = DHCP_OPTION_SERVER_ID;
  packet.dhcp.options[pos++] = 4;
  memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet.dhcp.options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet.udph.len = htons(udp_len);

  /* IP header */
  packet.iph.tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(&packet.iph);

  /* Calculate total length */
  length = udp_len + PKT_IP_HLEN + PKT_ETH_HLEN;

  return dhcp_send(this, &this->ipif, conn->hismac, &packet, length);
}

/**
 * dhcp_sendNAK()
 * Send of a DHCP negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a DHCP relay server)
 **/
int dhcp_sendNAK(struct dhcp_conn_t *conn, 
		 struct dhcp_fullpacket_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct dhcp_fullpacket_t packet;
  uint16_t length = 576 + 4; /* Maximum length */
  uint16_t udp_len = 576 - 20; /* Maximum length */
  size_t pos = 0;

  /* Get packet default values */
  pos = dhcp_create_pkt(DHCPNAK, &packet, pack, conn);

  /* DHCP Payload */

  /* Must be listening address */
  packet.dhcp.options[pos++] = DHCP_OPTION_SERVER_ID;
  packet.dhcp.options[pos++] = 4;
  memcpy(&packet.dhcp.options[pos], &conn->ourip.s_addr, 4);
  pos += 4;

  packet.dhcp.options[pos++] = DHCP_OPTION_END;

  /* UDP header */
  udp_len = pos + DHCP_MIN_LEN + PKT_UDP_HLEN;
  packet.udph.len = htons(udp_len);

  /* IP header */
  packet.iph.tot_len = htons(udp_len + PKT_IP_HLEN);

  /* Work out checksums */
  chksum(&packet.iph);

  /* Calculate total length */
  length = udp_len + PKT_IP_HLEN + PKT_ETH_HLEN;

  return dhcp_send(this, &this->ipif, conn->hismac, &packet, length);
}


/**
 *  dhcp_getreq()
 *  Process a received DHCP request and sends a response.
 **/
int dhcp_getreq(struct dhcp_t *this, struct dhcp_fullpacket_t *pack, size_t len) {
  uint8_t mac[PKT_ETH_ALEN];
  struct dhcp_tag_t *message_type = 0;
  struct dhcp_tag_t *requested_ip = 0;
  struct dhcp_conn_t *conn;
  struct in_addr addr;

  if (pack->udph.dst != htons(DHCP_BOOTPS)) 
    return 0; /* Not a DHCP packet */

  if (dhcp_gettag(&pack->dhcp, ntohs(pack->udph.len)-PKT_UDP_HLEN, 
		  &message_type, DHCP_OPTION_MESSAGE_TYPE)) {
    return -1;
  }

  if (message_type->l != 1)
    return -1; /* Wrong length of message type */

  if (pack->dhcp.giaddr)
    memcpy(mac, pack->dhcp.chaddr, PKT_ETH_ALEN);
  else
    memcpy(mac, pack->ethh.src, PKT_ETH_ALEN);

  switch(message_type->v[0]) {

  case DHCPRELEASE:
    dhcp_release_mac(this, mac, RADIUS_TERMINATE_CAUSE_LOST_CARRIER);

  case DHCPDISCOVER:
  case DHCPREQUEST:
  case DHCPINFORM:
    break;

  default:
    return 0; /* Unsupported message type */
  }

  if (this->relayfd > 0) {
    /** Relay the DHCP request **/
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = options.dhcpgwip.s_addr;
    addr.sin_port = htons(options.dhcpgwport);

    if (options.dhcprelayip.s_addr)
      pack->dhcp.giaddr = options.dhcprelayip.s_addr;
    else
      pack->dhcp.giaddr = options.uamlisten.s_addr;

    /* if we can't send, lets do dhcp ourselves */
    if (sendto(this->relayfd, &pack->dhcp, ntohs(pack->udph.len) - PKT_UDP_HLEN, 0, 
	       (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      log_err(errno, "could not relay DHCP request!");
    }
    else {
      return 0;
    }
  }

  if (message_type->v[0] == DHCPRELEASE) {
    /* No Reply to client is sent */
    return 0;
  }

  /* Check to see if we know MAC address. If not allocate new conn */
  if (dhcp_hashget(this, &conn, mac)) {
    
    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn) /* TODO: Should be deleted! */
      return 0; 

    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, mac)) /* TODO: Delete! */
      return 0; /* Out of connections */
  }

  if (conn->authstate == DHCP_AUTH_DROP)
    return 0;

  /* Request an IP address */ 
  if (conn->authstate == DHCP_AUTH_NONE) {
    addr.s_addr = pack->dhcp.ciaddr;
    if (this->cb_request)
      if (this->cb_request(conn, &addr, pack, len)) {
	return 0; /* Ignore request if IP address was not allocated */
      }
  }
  
  conn->lasttime = mainclock;

  /* Discover message */
  /* If an IP address was assigned offer it to the client */
  /* Otherwise ignore the request */
  if (message_type->v[0] == DHCPDISCOVER) {
    if (conn->hisip.s_addr) 
      dhcp_sendOFFER(conn, pack, len);
    return 0;
  }
  
  /* Request message */
  if (message_type->v[0] == DHCPREQUEST) {
    
    if (!conn->hisip.s_addr) {
      if (this->debug) log_dbg("hisip not set");
      return dhcp_sendNAK(conn, pack, len);
    }

    if (!memcmp(&conn->hisip.s_addr, &pack->dhcp.ciaddr, 4)) {
      if (this->debug) log_dbg("hisip match ciaddr");
      return dhcp_sendACK(conn, pack, len);
    }

    if (!dhcp_gettag(&pack->dhcp, ntohs(pack->udph.len)-PKT_UDP_HLEN, 
		     &requested_ip, DHCP_OPTION_REQUESTED_IP)) {
      if (!memcmp(&conn->hisip.s_addr, requested_ip->v, 4))
	return dhcp_sendACK(conn, pack, len);
    }

    if (this->debug) log_dbg("Sending NAK to client");
    return dhcp_sendNAK(conn, pack, len);
  }
  
  /* 
   *  Unsupported DHCP message: Ignore 
   */
  if (this->debug) log_dbg("Unsupported DHCP message ignored");
  return 0;
}


/**
 * dhcp_set_addrs()
 * Set various IP addresses of a connection.
 **/
int dhcp_set_addrs(struct dhcp_conn_t *conn, struct in_addr *hisip,
		   struct in_addr *hismask, struct in_addr *ourip,
		   struct in_addr *ourmask, struct in_addr *dns1,
		   struct in_addr *dns2, char *domain) {

  conn->hisip.s_addr = hisip->s_addr;
  conn->hismask.s_addr = hismask->s_addr;
  conn->ourip.s_addr = ourip->s_addr;
  conn->dns1.s_addr = dns1->s_addr;
  conn->dns2.s_addr = dns2->s_addr;

  if (domain) {
    strncpy(conn->domain, domain, DHCP_DOMAIN_LEN);
    conn->domain[DHCP_DOMAIN_LEN-1] = 0;
  }
  else {
    conn->domain[0] = 0;
  }

  if (options.uamanyip && 
      (hisip->s_addr & ourmask->s_addr) != (ourip->s_addr & ourmask->s_addr)) {
    /**
     *  We have enabled ''uamanyip'' and the address we are setting does
     *  not fit in ourip's network. In this case, add a route entry. 
     */
    struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
    if (appconn) {
      struct ippoolm_t *ipm = (struct ippoolm_t*)appconn->uplink;
      if (ipm && ipm->inuse == 2) {
	struct in_addr mask;
	int res;
	mask.s_addr = 0xffffffff;
	res = net_add_route(hisip, ourip, &mask);
	log_dbg("Adding route for %s %d", inet_ntoa(*hisip), res);
      }
    }
  }

  return 0;
}

static unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int dhcp_receive_ip(struct dhcp_t *this, struct pkt_ippacket_t *pack, size_t len) {
  struct pkt_tcphdr_t *tcph = (struct pkt_tcphdr_t*) pack->payload;
  /*struct pkt_udphdr_t *udph = (struct pkt_udphdr_t*) pack->payload;*/
  struct dhcp_conn_t *conn;
  struct in_addr ourip;
  struct in_addr addr;

  /*
   *  Received a packet from the dhcpif
   */

  if (this->debug)
    log_dbg("DHCP packet received");
  
  /* 
   *  Check that the destination MAC address is our MAC or Broadcast 
   */
  if ((memcmp(pack->ethh.dst, this->ipif.hwaddr, PKT_ETH_ALEN)) && 
      (memcmp(pack->ethh.dst, bmac, PKT_ETH_ALEN))) {
    log_dbg("dropping packet; not for our MAC or broadcast: "
	    "%2X:%2X:%2X:%2X:%2X:%2X", 
	    pack->ethh.dst[0], pack->ethh.dst[1],
	    pack->ethh.dst[2], pack->ethh.dst[3],
	    pack->ethh.dst[4], pack->ethh.dst[5]);
    return 0;
  }

  ourip.s_addr = this->ourip.s_addr;

  /* 
   *  DHCP (BOOTPS) packets for broadcast or us specifically
   */
  if (((pack->iph.daddr == 0) ||
       (pack->iph.daddr == 0xffffffff) ||
       (pack->iph.daddr == ourip.s_addr)) &&
      ((pack->iph.version_ihl == PKT_IP_VER_HLEN) && 
       (pack->iph.protocol == PKT_IP_PROTO_UDP) &&
       (((struct dhcp_fullpacket_t*)pack)->udph.dst == htons(DHCP_BOOTPS)))) {
    log_dbg("dhcp/bootps request being processed");
    return dhcp_getreq(this, (struct dhcp_fullpacket_t*) pack, len);
  }

  /* 
   *  Check to see if we know MAC address
   */
  if (!dhcp_hashget(this, &conn, pack->ethh.src)) {
    if (this->debug) log_dbg("Address found");
    ourip.s_addr = conn->ourip.s_addr;
  }
  else {
    /* ALPAPAD */
    struct in_addr reqaddr;
    /* Get local copy */
    memcpy(&reqaddr.s_addr, &pack->iph.saddr, PKT_IP_ALEN);

    if (options.debug) 
      log_dbg("Address not found (%s)", inet_ntoa(reqaddr)); 

    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn && !options.uamanyip)
      return 0; 

    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, pack->ethh.src)) {
      if (this->debug) 
	log_dbg("dropping packet; out of connections");
      return 0; /* Out of connections */
    }
  }

  /* Request an IP address 
  if (options.uamanyip && 
      conn->authstate == DHCP_AUTH_NONE) {
    this->cb_request(conn, &pack->iph.saddr);
  } */
  
  /* Return if we do not know peer */
  if (!conn) {
    if (this->debug) 
      log_dbg("dropping packet; no peer");
    return 0;
  }

  /* 
   *  Request an IP address 
   */
  if ((conn->authstate == DHCP_AUTH_NONE) && 
      (options.uamanyip || 
       ((pack->iph.daddr != 0) && 
	(pack->iph.daddr != 0xffffffff)))) {
    addr.s_addr = pack->iph.saddr;
    if (this->cb_request)
      if (this->cb_request(conn, &addr, 0, 0)) {
	if (this->debug) 
	  log_dbg("dropping packet; ip not known: %s", inet_ntoa(addr));
	return 0; /* Ignore request if IP address was not allocated */
      }
  }


  conn->lasttime = mainclock;

  /*
  if (((pack->iph.daddr == conn->dns1.s_addr) ||
       (pack->iph.daddr == conn->dns2.s_addr)) &&
      (pack->iph.protocol == PKT_IP_PROTO_UDP) &&
      (udph->dst == htons(DHCP_DNS))) {
    if (dhcp_checkDNS(conn, pack, len)) return 0;
    }*/

  /* Was it a request for the auto-logout service? */
  if ((pack->iph.daddr == options.uamlogout.s_addr) &&
      (pack->iph.protocol == PKT_IP_PROTO_TCP) &&
      (tcph->dst == htons(DHCP_HTTP))) {
    if (conn->peer) {
      struct app_conn_t *appconn = (struct app_conn_t *)conn->peer;
      if (appconn->s_state.authenticated) {
	terminate_appconn(appconn, RADIUS_TERMINATE_CAUSE_USER_REQUEST);
	if (options.debug)
	  log_dbg("Dropping session due to request for auto-logout ip");
	appconn->uamexit=1;
      }
    }
  }

  switch (conn->authstate) {
  case DHCP_AUTH_PASS:
    /* Check for post-auth proxy, otherwise pass packets unmodified */
    dhcp_postauthDNAT(conn, pack, len, 0);
    break; 

  case DHCP_AUTH_UNAUTH_TOS:
    /* Set TOS to specified value (unauthenticated) */
    pack->iph.tos = conn->unauth_cp;
    chksum(&pack->iph);
    break;

  case DHCP_AUTH_AUTH_TOS:
    /* Set TOS to specified value (authenticated) */
    pack->iph.tos = conn->auth_cp;
    chksum(&pack->iph);
    break;

  case DHCP_AUTH_SPLASH:
    dhcp_doDNAT(conn, pack, len);
    break;

  case DHCP_AUTH_DNAT:
    /* Destination NAT if request to unknown web server */
    if (dhcp_doDNAT(conn, pack, len)) {
      if (this->debug) log_dbg("dropping packet; not nat'ed");
      return 0; /* drop */
    }
    break;

  case DHCP_AUTH_DROP: 
  default:
    if (this->debug) 
      log_dbg("dropping packet; auth-drop");
    return 0;
  }

  /*done:*/

  if (options.usetap) {
    struct pkt_ethhdr_t *ethh = (struct pkt_ethhdr_t *)pack;
    memcpy(ethh->dst,tuntap(tun).hwaddr,PKT_ETH_ALEN);
  }

  if ((conn->hisip.s_addr) && (this->cb_data_ind)) {
    this->cb_data_ind(conn, pack, len);
  } else {
    if (this->debug) 
      log_dbg("no hisip; packet-drop");
  }
  
  return 0;
}

/**
 * Call this function when a new IP packet has arrived. This function
 * should be part of a select() loop in the application.
 **/
int dhcp_decaps(struct dhcp_t *this) {
  struct pkt_ippacket_t packet;
  ssize_t length;
  
  if ((length = net_read(&this->ipif, &packet, sizeof(packet))) < 0) 
    return -1;

  if (this->debug) {
    struct pkt_ethhdr_t *ethh = &packet.ethh;
    log_dbg("dhcp_decaps: dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x prot=%.4x",
	    ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
	    ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
	    ntohs(ethh->prot));
  }

  return dhcp_receive_ip(this, &packet, length);
}

int dhcp_relay_decaps(struct dhcp_t *this) {
  struct dhcp_tag_t *message_type = 0;
  struct dhcp_fullpacket_t fullpack;
  struct dhcp_conn_t *conn;
  struct dhcp_packet_t packet;
  struct sockaddr_in addr;
  socklen_t fromlen = sizeof(addr);
  ssize_t length;


  if ((length = recvfrom(this->relayfd, &packet, sizeof(packet), 0,
                         (struct sockaddr *) &addr, &fromlen)) <= 0) {
    log_err(errno, "recvfrom() failed");
    return -1;
  }

  log_dbg("DHCP relay response of length %d received", length);

  if (addr.sin_addr.s_addr != options.dhcpgwip.s_addr) {
    log_err(0, "received DHCP response from host other than our gateway");
    return -1;
  }

  if (addr.sin_port != htons(options.dhcpgwport)) {
    log_err(0, "received DHCP response from port other than our gateway");
    return -1;
  }

  if (dhcp_gettag(&packet, length, &message_type, DHCP_OPTION_MESSAGE_TYPE)) {
    log_err(0, "no message type");
    return -1;
  }
  
  if (message_type->l != 1) {
    log_err(0, "wrong message type length");
    return -1; /* Wrong length of message type */
  }
  
  if (dhcp_hashget(this, &conn, packet.chaddr)) {

    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, packet.chaddr)) {
      log_err(0, "out of connections");
      return 0; /* Out of connections */
    }

    this->cb_request(conn, (struct in_addr *)&packet.yiaddr, 0, 0);
  }

  packet.giaddr = 0;

  memset(&fullpack, 0, sizeof(fullpack));

  memcpy(fullpack.ethh.dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(fullpack.ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);
  fullpack.ethh.prot = htons(PKT_ETH_PROTO_IP);

  fullpack.iph.version_ihl = PKT_IP_VER_HLEN;
  fullpack.iph.tot_len = htons(length + PKT_UDP_HLEN + PKT_IP_HLEN);
  fullpack.iph.ttl = 0x10;
  fullpack.iph.protocol = 0x11;

  fullpack.iph.saddr = conn->ourip.s_addr;
  fullpack.udph.src = htons(DHCP_BOOTPS);
  fullpack.udph.len = htons(length + PKT_UDP_HLEN);

  /*if (fullpack.dhcp.ciaddr) {
    fullpack.udph.daddr = req->dhcp.ciaddr; 
    fullpack.udph.dst = htons(DHCP_BOOTPC);
  } else if (req->dhcp.giaddr) {
    fullpack.iph.daddr = req->dhcp.giaddr; 
    fullpack.udph.dst = htons(DHCP_BOOTPS);
    } else */

  if (message_type->v[0] == DHCPNAK || packet.flags[0] & 0x80) {
    fullpack.iph.daddr = ~0; 
    fullpack.udph.dst = htons(DHCP_BOOTPC);
    fullpack.dhcp.flags[0] = 0x80;
  } if (packet.ciaddr) {
    fullpack.iph.daddr = packet.ciaddr; 
    fullpack.udph.dst = htons(DHCP_BOOTPC);
  } else {
    fullpack.iph.daddr = packet.yiaddr; 
    fullpack.udph.dst = htons(DHCP_BOOTPC);
  }

  memcpy(&fullpack.dhcp, &packet, sizeof(packet));

  { /* rewrite the server-id, otherwise will not get subsequent requests */
    struct dhcp_tag_t *tag = 0;
    if (!dhcp_gettag(&fullpack.dhcp, length, &tag, DHCP_OPTION_SERVER_ID)) {
      memcpy(tag->v, &conn->ourip.s_addr, 4);
    }
  }

  chksum(&fullpack.iph);

  return dhcp_send(this, &this->ipif, conn->hismac, &fullpack, 
		   length + PKT_UDP_HLEN + PKT_IP_HLEN + PKT_ETH_HLEN);
}

/**
 * dhcp_data_req()
 * Call this function to send an IP packet to the peer.
 * Called from the tun_ind function. This method is passed either
 * an Ethernet frame or an IP packet. 
 **/
int dhcp_data_req(struct dhcp_conn_t *conn, void *pack, size_t len, int ethhdr) {
  struct dhcp_t *this = conn->parent;
  struct pkt_ippacket_t packet;
  size_t length = len;

  if (ethhdr) { /* Ethernet frame */
    memcpy(&packet, pack, len);
  } else {      /* IP packet */
    memcpy(&packet.iph, pack, len);
    length += PKT_ETH_HLEN;
  }

  /* Ethernet header */
  memcpy(packet.ethh.dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(packet.ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);
  packet.ethh.prot = htons(PKT_ETH_PROTO_IP);
  
  switch (conn->authstate) {

  case DHCP_AUTH_PASS:
  case DHCP_AUTH_AUTH_TOS:
    dhcp_postauthDNAT(conn, &packet, length, 1);
    break;

  case DHCP_AUTH_SPLASH:
  case DHCP_AUTH_UNAUTH_TOS:
    dhcp_undoDNAT(conn, &packet, &length);
    break;

  case DHCP_AUTH_DNAT:
    /* undo destination NAT */
    if (dhcp_undoDNAT(conn, &packet, &length)) {
      if (this->debug) log_dbg("dhcp_undoDNAT() returns true");
      return 0;
    }
    break;

  case DHCP_AUTH_DROP: 
  default: return 0;
  }

  return dhcp_send(this, &this->ipif, conn->hismac, &packet, length);
}


/**
 * dhcp_sendARP()
 * Send ARP message to peer
 **/
static int
dhcp_sendARP(struct dhcp_conn_t *conn, struct arp_fullpacket_t *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct arp_fullpacket_t packet;
  struct in_addr reqaddr;
  size_t length = sizeof(packet);

  /* Get local copy */
  memcpy(&reqaddr.s_addr, pack->arp.tpa, PKT_IP_ALEN);

  /* Check that request is within limits */

  /* Get packet default values */
  memset(&packet, 0, sizeof(packet));
	 
  /* ARP Payload */
  packet.arp.hrd = htons(DHCP_HTYPE_ETH);
  packet.arp.pro = htons(PKT_ETH_PROTO_IP);
  packet.arp.hln = PKT_ETH_ALEN;
  packet.arp.pln = PKT_IP_ALEN;
  packet.arp.op  = htons(DHCP_ARP_REPLY);

  /* Source address */
  memcpy(packet.arp.sha, this->arpif.hwaddr, PKT_ETH_ALEN);
  memcpy(packet.arp.spa, &reqaddr.s_addr, PKT_IP_ALEN);

  /* Target address */
  memcpy(packet.arp.tha, &conn->hismac, PKT_ETH_ALEN);
  memcpy(packet.arp.tpa, &conn->hisip.s_addr, PKT_IP_ALEN);

  /* Ethernet header */
  memcpy(packet.ethh.dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(packet.ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);
  packet.ethh.prot = htons(PKT_ETH_PROTO_ARP);

  return dhcp_send(this, &this->arpif, conn->hismac, &packet, length);
}


int dhcp_receive_arp(struct dhcp_t *this, 
		     struct arp_fullpacket_t *pack, size_t len) {
  
  unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  struct dhcp_conn_t *conn;
  struct in_addr reqaddr;
  struct in_addr taraddr;

  /* Check that this is ARP request */
  if (pack->arp.op != htons(DHCP_ARP_REQUEST)) {
    if (this->debug)
      log_dbg("Received other ARP than request!");
    return 0;
  }

  /* Check that MAC address is our MAC or Broadcast */
  if ((memcmp(pack->ethh.dst, this->ipif.hwaddr, PKT_ETH_ALEN)) && 
      (memcmp(pack->ethh.dst, bmac, PKT_ETH_ALEN))) {
    if (this->debug) 
      log_dbg("Received ARP request for other destination!");
    return 0;
  }

  /* get sender IP address */
  memcpy(&reqaddr.s_addr, &pack->arp.spa, PKT_IP_ALEN);

  /* get target IP address */
  memcpy(&taraddr.s_addr, &pack->arp.tpa, PKT_IP_ALEN);


  /* Check to see if we know MAC address. */
  if (dhcp_hashget(this, &conn, pack->ethh.src)) {
    if (options.debug) 
      log_dbg("Address not found: %s", inet_ntoa(reqaddr));

    /* Do we allow dynamic allocation of IP addresses? */
    if (!this->allowdyn && !options.uamanyip) {
      if (this->debug) 
	log_dbg("ARP: Unknown client and no dynip: %s", inet_ntoa(taraddr));
      return 0; 
    }
    
    /* Allocate new connection */
    if (dhcp_newconn(this, &conn, pack->ethh.src)) {
      log_warn(0, "ARP: out of connections");
      return 0; /* Out of connections */
    }
  }

  if (conn->authstate == DHCP_AUTH_DROP) 
    return 0;
  
  /* if no sender ip, then client is checking their own ip */
  if (!reqaddr.s_addr) {
    /* XXX: lookup in ippool to see if we really do know who has this */
    /* XXX: it should also ack if *we* are that ip */
    if (this->debug) 
      log_dbg("ARP: Ignoring self-discovery: %s", inet_ntoa(taraddr));

    /* If a static ip address... */
    this->cb_request(conn, &taraddr, 0, 0);

    return 0;
  }

  if (!memcmp(&reqaddr.s_addr, &taraddr.s_addr, 4)) { 

    /* Request an IP address */
    if (options.uamanyip /*or static ip*/ &&
	conn->authstate == DHCP_AUTH_NONE) {
      this->cb_request(conn, &reqaddr, 0, 0);
    } 

    if (this->debug)
      log_dbg("ARP: gratuitous arp %s!", inet_ntoa(taraddr));

    return 0;
  }

  if (!conn->hisip.s_addr && !options.uamanyip) {
    if (this->debug)
      log_dbg("ARP: request did not come from known client!");
    return 0; /* Only reply if he was allocated an address */
  }

  /* Is ARP request for clients own address: Ignore */
  if (conn->hisip.s_addr == taraddr.s_addr) {
    if (this->debug)
      log_dbg("ARP: hisip equals target ip: %s!",
	      inet_ntoa(conn->hisip));
    return 0;
  }
  
  if (!options.uamanyip) {
    /* If ARP request outside of mask: Ignore */
    if (reqaddr.s_addr &&
	(conn->hisip.s_addr & conn->hismask.s_addr) !=
	(reqaddr.s_addr & conn->hismask.s_addr)) {
      if (this->debug) 
	log_dbg("ARP: request not in our subnet");
      return 0;
    }
    
    if (memcmp(&conn->ourip.s_addr, &taraddr.s_addr, 4)) { /* if ourip differs from target ip */
      if (options.debug) {
	log_dbg("ARP: Did not ask for router address: %s", inet_ntoa(conn->ourip));
	log_dbg("ARP: Asked for target: %s", inet_ntoa(taraddr));
      }
      return 0; /* Only reply if he asked for his router address */
    }
  }
  else if ((taraddr.s_addr != options.dhcplisten.s_addr) &&
          ((taraddr.s_addr & options.mask.s_addr) == options.net.s_addr)) {
    /* when uamanyip is on we should ignore arp requests that ARE within our subnet except of course the ones for ourselves */
    if (options.debug)
      log_dbg("ARP: request for IP=%s other than us within our subnet(uamanyip on), ignoring", inet_ntoa(taraddr));
    return 0;
  }

  conn->lasttime = mainclock;

  dhcp_sendARP(conn, pack, len);

  return 0;
}


/**
 * dhcp_arp_ind()
 * Call this function when a new ARP packet has arrived. This function
 * should be part of a select() loop in the application.
 **/
int dhcp_arp_ind(struct dhcp_t *this)  /* ARP Indication */
{
  struct arp_fullpacket_t packet;
  ssize_t length;
  
  if ((length = net_read(&this->arpif, &packet, sizeof(packet))) < 0) 
    return -1;

  if (options.debug) {
    struct pkt_ethhdr_t *ethh = &packet.ethh;
    log_dbg("arp_decaps: dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x prot=%.4x",
	    ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
	    ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
	    ntohs(ethh->prot));
  }

  dhcp_receive_arp(this, &packet, length);

  return 0;
}


/**
 * eapol_sendNAK()
 * Send of a EAPOL negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a EAPOL relay server)
 **/
int dhcp_senddot1x(struct dhcp_conn_t *conn,  
		   struct dot1xpacket_t *pack, size_t len) {
  struct dhcp_t *this = conn->parent;
  return dhcp_send(this, &this->eapif, conn->hismac, pack, len);
}

/**
 * eapol_sendNAK()
 * Send of a EAPOL negative acknowledge message to a peer.
 * NAK messages are always sent to broadcast IP address (
 * except when using a EAPOL relay server)
 **/
int dhcp_sendEAP(struct dhcp_conn_t *conn, void *pack, size_t len) {

  struct dhcp_t *this = conn->parent;
  struct dot1xpacket_t packet;

  /* Ethernet header */
  memcpy(packet.ethh.dst, conn->hismac, PKT_ETH_ALEN);
  memcpy(packet.ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);
  packet.ethh.prot = htons(PKT_ETH_PROTO_EAPOL);
  
  /* 802.1x header */
  packet.dot1x.ver  = 1;
  packet.dot1x.type = 0; /* EAP */
  packet.dot1x.len =  htons((uint16_t)len);

  memcpy(&packet.eap, pack, len);
  
  return dhcp_send(this, &this->eapif, conn->hismac, &packet, (PKT_ETH_HLEN + 4 + len));
}

int dhcp_sendEAPreject(struct dhcp_conn_t *conn, void *pack, size_t len) {

  /*struct dhcp_t *this = conn->parent;*/

  struct eap_packet_t packet;

  if (pack) {
    dhcp_sendEAP(conn, pack, len);
  }
  else {
    memset(&packet, 0, sizeof(packet));
    packet.code      =  4;
    packet.id        =  1; /* TODO ??? */
    packet.length    =  htons(4);
  
    dhcp_sendEAP(conn, &packet, 4);
  }

  return 0;

}

int dhcp_receive_eapol(struct dhcp_t *this, struct dot1xpacket_t *pack) {
  struct dhcp_conn_t *conn = NULL;
  unsigned char const bmac[PKT_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  unsigned char const amac[PKT_ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};

  /* Check to see if we know MAC address. */
  if (!dhcp_hashget(this, &conn, pack->ethh.src)) {
    if (this->debug) log_dbg("Address found");
  }
  else {
    if (this->debug) log_dbg("Address not found");
  }
  
  if (this->debug) 
    log_dbg("IEEE 802.1x Packet: %.2x, %.2x %d",
	    pack->dot1x.ver, pack->dot1x.type,
	    ntohs(pack->dot1x.len));
  
  /* Check that MAC address is our MAC, Broadcast or authentication MAC */
  if ((memcmp(pack->ethh.dst, this->ipif.hwaddr, PKT_ETH_ALEN)) && 
      (memcmp(pack->ethh.dst, bmac, PKT_ETH_ALEN)) && 
      (memcmp(pack->ethh.dst, amac, PKT_ETH_ALEN)))
    return 0;
  
  if (pack->dot1x.type == 1) { /* Start */
    struct dot1xpacket_t p;
    memset(&p, 0, sizeof(p));
    
    /* Allocate new connection */
    if (conn == NULL) {
      if (dhcp_newconn(this, &conn, pack->ethh.src))
	return 0; /* Out of connections */
    }

    /* Ethernet header */
    memcpy(p.ethh.dst, pack->ethh.src, PKT_ETH_ALEN);
    memcpy(p.ethh.src, this->ipif.hwaddr, PKT_ETH_ALEN);
    p.ethh.prot = htons(PKT_ETH_PROTO_EAPOL);

    /* 802.1x header */
    p.dot1x.ver  = 1;
    p.dot1x.type = 0; /* EAP */
    p.dot1x.len =  htons(5);
    
    /* EAP Packet */
    p.eap.code      =  1;
    p.eap.id        =  1;
    p.eap.length    =  htons(5);
    p.eap.type      =  1; /* Identity */

    dhcp_senddot1x(conn, &p, PKT_ETH_HLEN + 4 + 5);
    return 0;
  }
  else if (pack->dot1x.type == 0) { /* EAP */

    /* TODO: Currently we only support authentications starting with a
       client sending a EAPOL start message. Need to also support
       authenticator initiated communications. */
    if (!conn)
      return 0;

    conn->lasttime = mainclock;
    
    if (this->cb_eap_ind)
      this->cb_eap_ind(conn, &pack->eap, ntohs(pack->eap.length));

    return 0;
  }
  else { /* Check for logoff */
    return 0;
  }
}

/**
 * dhcp_eapol_ind()
 * Call this function when a new EAPOL packet has arrived. This function
 * should be part of a select() loop in the application.
 **/
int dhcp_eapol_ind(struct dhcp_t *this) {
  struct dot1xpacket_t packet;
  ssize_t length;
  
  if ((length = net_read(&this->eapif, &packet, sizeof(packet))) < 0) 
    return -1;

  if (options.debug) {
    struct pkt_ethhdr_t *ethh = &packet.ethh;
    log_dbg("eapol_decaps: dst=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x src=%.2x:%.2x:%.2x:%.2x:%.2x:%.2x prot=%.4x",
	    ethh->dst[0],ethh->dst[1],ethh->dst[2],ethh->dst[3],ethh->dst[4],ethh->dst[5],
	    ethh->src[0],ethh->src[1],ethh->src[2],ethh->src[3],ethh->src[4],ethh->src[5],
	    ntohs(ethh->prot));
  }

  return dhcp_receive_eapol(this, &packet);
}


/**
 * dhcp_set_cb_eap_ind()
 * Set callback function which is called when packet has arrived
 * Used for eap packets
 **/
int dhcp_set_cb_eap_ind(struct dhcp_t *this, 
  int (*cb_eap_ind) (struct dhcp_conn_t *conn, void *pack, size_t len)) {
  this->cb_eap_ind = cb_eap_ind;
  return 0;
}


/**
 * dhcp_set_cb_data_ind()
 * Set callback function which is called when packet has arrived
 **/
int dhcp_set_cb_data_ind(struct dhcp_t *this, 
  int (*cb_data_ind) (struct dhcp_conn_t *conn, void *pack, size_t len)) {
  this->cb_data_ind = cb_data_ind;
  return 0;
}


/**
 * dhcp_set_cb_data_ind()
 * Set callback function which is called when a dhcp request is received
 **/
int dhcp_set_cb_request(struct dhcp_t *this, 
  int (*cb_request) (struct dhcp_conn_t *conn, struct in_addr *addr, struct dhcp_fullpacket_t *pack, size_t len)) {
  this->cb_request = cb_request;
  return 0;
}


/**
 * dhcp_set_cb_connect()
 * Set callback function which is called when a connection is created
 **/
int dhcp_set_cb_connect(struct dhcp_t *this, 
             int (*cb_connect) (struct dhcp_conn_t *conn)) {
  this->cb_connect = cb_connect;
  return 0;
}

/**
 * dhcp_set_cb_disconnect()
 * Set callback function which is called when a connection is deleted
 **/
int dhcp_set_cb_disconnect(struct dhcp_t *this, 
  int (*cb_disconnect) (struct dhcp_conn_t *conn, int term_cause)) {
  this->cb_disconnect = cb_disconnect;
  return 0;
}

int dhcp_set_cb_getinfo(struct dhcp_t *this, 
  int (*cb_getinfo) (struct dhcp_conn_t *conn, bstring b, int fmt)) {
  this->cb_getinfo = cb_getinfo;
  return 0;
}


#if defined (__FreeBSD__) || defined (__APPLE__) || defined (__OpenBSD__)

int dhcp_receive(struct dhcp_t *this) {
  ssize_t length = 0;
  size_t offset = 0;
  struct bpf_hdr *hdrp;
  struct pkt_ethhdr_t *ethhdr;
  
  if (this->rbuf_offset == this->rbuf_len) {
    length = net_read(&this->ipif, this->rbuf, this->rbuf_max);

    if (length <= 0)
      return length;

    this->rbuf_offset = 0;
    this->rbuf_len = length;
  }
  
  while (this->rbuf_offset != this->rbuf_len) {
    
    if (this->rbuf_len - this->rbuf_offset < sizeof(struct bpf_hdr)) {
      this->rbuf_offset = this->rbuf_len;
      continue;
    }
    
    hdrp = (struct bpf_hdr *) &this->rbuf[this->rbuf_offset];
    
    if (this->rbuf_offset + hdrp->bh_hdrlen + hdrp->bh_caplen > 
	this->rbuf_len) {
      this->rbuf_offset = this->rbuf_len;
      continue;
    }

    if (hdrp->bh_caplen != hdrp->bh_datalen) {
      this->rbuf_offset += hdrp->bh_hdrlen + hdrp->bh_caplen;
      continue;
    }

    ethhdr = (struct pkt_ethhdr_t *) 
      (this->rbuf + this->rbuf_offset + hdrp->bh_hdrlen);

    switch (ntohs(ethhdr->prot)) {
    case PKT_ETH_PROTO_IP:
      dhcp_receive_ip(this, (struct pkt_ippacket_t*) ethhdr, hdrp->bh_caplen);
      break;
    case PKT_ETH_PROTO_ARP:
      dhcp_receive_arp(this, (struct arp_fullpacket_t*) ethhdr, hdrp->bh_caplen);
      break;
    case PKT_ETH_PROTO_EAPOL:
      dhcp_receive_eapol(this, (struct dot1xpacket_t*) ethhdr);
      break;

    default:
      break;
    }
    this->rbuf_offset += hdrp->bh_hdrlen + hdrp->bh_caplen;
  };
  return (0);
}
#endif
