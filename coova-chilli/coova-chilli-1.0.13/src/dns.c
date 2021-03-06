/*
 * DNS library functions
 * Copyright (c) 2006-2008 David Bird <david@coova.com>
 *
 * The contents of this file may be used under the terms of the GNU
 * General Public License Version 2, provided that the above copyright
 * notice and this permission notice is included in all copies or
 * substantial portions of the software.
 *
 */

#include "dns.h"
#include "garden.h"
#include "syserr.h"
#include "dhcp.h"
#include "options.h"

#define antidnstunnel options.dnsparanoia

extern struct dhcp_t *dhcp;

char *
dns_fullname(char *data, size_t dlen, uint8_t *res, uint8_t *opkt, size_t olen, int lvl) {
  char *d = data;
  unsigned short l;
  
  if (lvl >= 15) 
    return 0;
  
  while ((l = *res++) != 0) {
    if ((l & 0xC0) == 0xC0) {
      unsigned short offset = ((l & ~0xC0) << 8) + *res;
      if (offset > olen) {
	log_dbg("bad value");
	return 0;
      }
      /*log_dbg("skip[%d]\n",offset);*/
      dns_fullname(d, dlen, opkt + (size_t)offset, opkt, olen, lvl+1);
      break;
    }
    
    if (l >= dlen) {
      log_dbg("bad value");
      return 0;
    }
    
    /*log_dbg("part[%.*s]\n",l,res);*/
    
    memcpy(d, res, l);
    d += l; dlen -= l;
    res += l;
    
    *d = '.';
    d += 1; dlen -= 1;
  }
  
  if (!lvl && data[strlen((char*)data)-1]=='.')
    data[strlen((char*)data)-1]=0;
  
  return data;
}

int dns_getname(uint8_t **pktp, size_t *left, char *name, size_t namesz, size_t *nameln) {
  size_t namelen = *nameln;
  uint8_t *p_pkt = *pktp;
  size_t len = *left;
  uint8_t l;
  
  namelen = 0;
  while (len-- && (l = name[namelen++] = *p_pkt++) != 0) {
    if ((l & 0xC0) == 0xC0) {
      if (namelen >= namesz) {
	log_err(0, "name too long in DNS packet");
	break;
      }
      name[namelen++] = *p_pkt++;
      len--;
      break;
    }
  }
  
  *pktp = p_pkt;
  *nameln = namelen;
  *left = len;
  
  if (!len) {
    log_err(0, "failed to parse DNS packet");
    return -1;
  }
  
  return 0;
}

static void 
add_A_to_garden(uint8_t *p) {
  struct in_addr reqaddr;
  pass_through pt;
  memcpy(&reqaddr.s_addr, p, 4);
  memset(&pt, 0, sizeof(pass_through));
  pt.mask.s_addr = 0xffffffff;
  pt.host = reqaddr;
  if (pass_through_add(dhcp->pass_throughs,
		       MAX_PASS_THROUGHS,
		       &dhcp->num_pass_throughs,
		       &pt))
    ;
}

int 
dns_copy_res(int q, 
	     uint8_t **pktp, size_t *left, 
	     uint8_t *opkt,  size_t olen, 
	     char *question, size_t qsize) {

#define return_error \
{ log_dbg("%s:%d: failed parsing DNS packet",__FILE__,__LINE__); return -1; }

  uint8_t *p_pkt = *pktp;
  size_t len = *left;
  
  uint8_t name[PKT_IP_PLEN];
  size_t namelen = 0;

  uint16_t type;
  uint16_t class;
  uint32_t ttl;
  uint16_t rdlen;

  uint32_t ul;
  uint16_t us;

  if (dns_getname(&p_pkt, &len, (char *)name, sizeof(name), &namelen)) 
    return_error;

  if (antidnstunnel && namelen > 128) {
    log_warn(0,"dropping dns for anti-dnstunnel (namelen: %d)",namelen);
    return -1;
  }

  if (len < 4) 
    return_error;

  memcpy(&us, p_pkt, sizeof(us));
  type = ntohs(us);
  p_pkt += 2;
  len -= 2;
  
  memcpy(&us, p_pkt, sizeof(us));
  class = ntohs(us);
  p_pkt += 2;
  len -= 2;
  
  log_dbg("It was a dns record type: %d class: %d", type, class);


  /* if dnsparanoia, checks here */

  if (antidnstunnel) {
    switch (type) {
    case 1:/* A */ 
      log_dbg("A record");
      break;
    case 5:/* CNAME */ 
      log_dbg("CNAME record");
      break;
    default:
      if (options.debug) switch(type) {
      case 6:  log_dbg("SOA record"); break;
      case 12: log_dbg("PTR record"); break;
      case 15: log_dbg("MX record");  break;
      case 16: log_dbg("TXT record"); break;
      default: log_dbg("Record type %d", type); break;
      }
      log_warn(0, "dropping dns for anti-dnstunnel (type %d: length %d)", type, rdlen);
      return -1;
    }
  }

  if (q) {
    dns_fullname(question, qsize, *pktp, opkt, olen, 0);
    
    log_dbg("Q: %s", question);

    *pktp = p_pkt;
    *left = len;

    return 0;
  } 

  if (len < 6) 
    return_error;
  
  memcpy(&ul, p_pkt, sizeof(ul));
  ttl = ntohl(ul);
  p_pkt += 4;
  len -= 4;
  
  memcpy(&us, p_pkt, sizeof(us));
  rdlen = ntohs(us);
  p_pkt += 2;
  len -= 2;
  
  /*log_dbg("-> w ttl: %d rdlength: %d/%d", ttl, rdlen, len);*/
  
  if (len < rdlen)
    return_error;
  
  /*
   *  dns records 
   */  
  
  switch (type) {
    
  case 1:/* A */
    log_dbg("A record");
    if (options.uamdomains) {
      int id;
      for (id=0; options.uamdomains[id]; id++) {

	log_dbg("checking %s [%s]", options.uamdomains[id], question);

	if (strlen(question) >= strlen(options.uamdomains[id]) &&
	    !strcmp(options.uamdomains[id],
		    question + (strlen(question) - strlen(options.uamdomains[id])))) {
	  size_t offset;
	  for (offset=0; offset < rdlen; offset += 4) {
	    add_A_to_garden(p_pkt+offset);
	  }

	  break;
	}
      }
    }
    break;

  case 5:/* CNAME */
    {
      char cname[256];
      memset(cname,0,sizeof(cname));
      dns_fullname(cname, sizeof(cname)-1, p_pkt, opkt, olen, 0);
      log_dbg("CNAME record %s", cname);
    }
    break;

  default:

    if (options.debug) switch(type) {
    case 6:  log_dbg("SOA record"); break;
    case 12: log_dbg("PTR record"); break;
    case 15: log_dbg("MX record");  break;
    case 16: log_dbg("TXT record"); break;
    default: log_dbg("Record type %d", type); break;
    }

    if (antidnstunnel) {
      log_warn(0, "dropping dns for anti-dnstunnel (type %d: length %d)", type, rdlen);
      return -1;
    }

    break;
  }
  
  p_pkt += rdlen;
  len -= rdlen;
  
  *pktp = p_pkt;
  *left = len;

  return 0;
}
