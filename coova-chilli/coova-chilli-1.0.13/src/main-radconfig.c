/* 
 *
 * chilli - ChilliSpot.org. A Wireless LAN Access Point Controller.
 * Copyright (C) 2003, 2004, 2005 Mondru AB.
 * Copyright (C) 2006 PicoPoint B.V.
 * Copyright (c) 2006-2007 David Bird <david@coova.com>
 *
 * The contents of this file may be used under the terms of the GNU
 * General Public License Version 2, provided that the above copyright
 * notice and this permission notice is included in all copies or
 * substantial portions of the software.
 * 
 */

#include "system.h"
#include "syserr.h"
#include "cmdline.h"
#include "dhcp.h"
#include "radius.h"
#include "radius_chillispot.h"
#include "radius_wispr.h"
#include "redir.h"
#include "chilli.h"
#include "options.h"

#define ADMIN_TIMEOUT 10

static int chilliauth_cb(struct radius_t *radius,
			 struct radius_packet_t *pack,
			 struct radius_packet_t *pack_req, void *cbp) {
  struct radius_attr_t *attr = NULL;
  /*char attrs[RADIUS_ATTR_VLEN+1];*/
  size_t offset = 0;

  if (!pack) { 
    sys_err(LOG_ERR, __FILE__, __LINE__, 0, "Radius request timed out");
    return 0;
  }

  if ((pack->code != RADIUS_CODE_ACCESS_REJECT) && 
      (pack->code != RADIUS_CODE_ACCESS_CHALLENGE) &&
      (pack->code != RADIUS_CODE_ACCESS_ACCEPT)) {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0, 
	    "Unknown radius access reply code %d", pack->code);
    return 0;
  }

  /* ACCESS-ACCEPT */
  if (pack->code != RADIUS_CODE_ACCESS_ACCEPT) {
    sys_err(LOG_ERR, __FILE__, __LINE__, 0, "Administrative-User Login Failed");
    return 0;
  }

  while (!radius_getnextattr(pack, &attr, 
			     RADIUS_ATTR_VENDOR_SPECIFIC,
			     RADIUS_VENDOR_CHILLISPOT,
			     RADIUS_ATTR_CHILLISPOT_CONFIG, 
			     0, &offset)) {
    char value[RADIUS_ATTR_VLEN+1] = "";
    strncpy(value, (const char *)attr->v.t, attr->l - 2);
    printf("%s\n", value);
  }

  return 0;
  
}

int static chilliauth() {
  unsigned char hwaddr[6];
  struct radius_t *radius=0;
  struct timeval idleTime;
  int endtime, now;
  int maxfd = 0;
  fd_set fds;
  int status;
  int ret=-1;

  if (!options.adminuser || !options.adminpasswd) return 1;

  if (radius_new(&radius, &options.radiuslisten, 0, 0, NULL, 0, NULL, NULL, NULL)) {
    log_err(0, "Failed to create radius");
    return ret;
  }

  /* get dhcpif mac */
  memset(hwaddr, 0, sizeof(hwaddr));

#ifdef SIOCGIFHWADDR
  if (!options.nasmac && options.dhcpif) {
    struct ifreq ifr;
    int fd;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
      memset(&ifr, 0, sizeof(ifr));
      strncpy(ifr.ifr_name, options.dhcpif, IFNAMSIZ);
      if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
	log_err(errno, "ioctl(d=%d, request=%d) failed", fd, SIOCGIFHWADDR);
      }
      memcpy(hwaddr, ifr.ifr_hwaddr.sa_data, PKT_ETH_ALEN);
      close(fd);
    }
  }
#endif

  radius_set(radius, hwaddr, (options.debug & DEBUG_RADIUS));
  radius_set_cb_auth_conf(radius, chilliauth_cb); 

  ret = chilliauth_radius(radius);

  if (radius->fd <= 0) {
    log_err(0, "not a valid socket!");
    return ret;
  } 

  maxfd = radius->fd;

  now = time(NULL);
  endtime = now + ADMIN_TIMEOUT; 

  while (endtime > now) {

    FD_ZERO(&fds);
    FD_SET(radius->fd, &fds);
    
    idleTime.tv_sec = 0;
    idleTime.tv_usec = REDIR_RADIUS_SELECT_TIME;
    radius_timeleft(radius, &idleTime);

    switch (status = select(maxfd + 1, &fds, NULL, NULL, &idleTime)) {
    case -1:
      sys_err(LOG_ERR, __FILE__, __LINE__, errno, "select() returned -1!");
      break;  
    case 0:
      radius_timeout(radius);
    default:
      break;
    }

    if (status > 0) {
      if (FD_ISSET(radius->fd, &fds)) {
	if (radius_decaps(radius) < 0) {
	  sys_err(LOG_ERR, __FILE__, __LINE__, 0, "radius_ind() failed!");
	}
	else {
	  ret = 0;
	}
	break;
      }
    }

    now = time(NULL);
  }  

  radius_free(radius);
  return ret;
}

int main(int argc, char **argv)
{
  if (process_options(argc, argv, 1))
    exit(1);
  
  return chilliauth();
}
