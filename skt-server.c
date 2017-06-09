#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <qrencode.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdbool.h>
#include <unistd.h>
#include <gpgme.h>

const char * psk_id_hint = "openpgp-skt";
const char schema[] = "OPENPGP+SKT";
const char priority[] = "NORMAL:-CTYPE-ALL"
  ":%SERVER_PRECEDENCE:%NO_TICKETS"
  ":-VERS-TLS1.0:-VERS-TLS1.1:-VERS-DTLS1.0:-VERS-DTLS1.2"
  ":-CURVE-SECP224R1:-CURVE-SECP192R1"
  ":-SIGN-ALL"
  ":-KX-ALL:+ECDHE-PSK:+DHE-PSK"
  ":-3DES-CBC:-CAMELLIA-128-CBC:-CAMELLIA-256-CBC";

#define PSK_BYTES 16
#define LOG_LEVEL 4

struct session_status {
  int listen_socket;
  int accepted_socket;
  gnutls_datum_t psk;
  char addrp[INET6_ADDRSTRLEN];
  int port;
  char caddrp[INET6_ADDRSTRLEN];
  int cport;
  char pskhex[PSK_BYTES*2 + 1];
  struct sockaddr_storage sa_serv_storage;
  struct sockaddr_storage sa_cli_storage;
  socklen_t sa_serv_storage_sz;
  socklen_t sa_cli_storage_sz;
  gnutls_session_t session;
  gpgme_ctx_t gpgctx;
  struct ifaddrs *ifap;
};


struct session_status * session_status_new() {
  struct session_status *status = calloc(1, sizeof(struct session_status));
  if (status) {
    status->sa_serv_storage_sz = sizeof (status->sa_serv_storage);
    status->sa_cli_storage_sz = sizeof (status->sa_cli_storage);
  }
  return status;
}

void session_status_free(struct session_status *status) {
  if (status) {
    if (status->gpgctx)
      gpgme_release(status->gpgctx);
    if (status->session) {
      gnutls_bye(status->session, GNUTLS_SHUT_RDWR);
      gnutls_deinit(status->session);
    }
    if (status->ifap)
      freeifaddrs(status->ifap);
    free(status);
  }
}


int print_qrcode(FILE* f, const QRcode* qrcode);


int get_psk_creds(gnutls_session_t session, const char* username, gnutls_datum_t* key) {
  struct session_status *status;
  status = gnutls_session_get_ptr(session);
  
  if (LOG_LEVEL > 2)
    fprintf(stderr, "sent username: %s, PSK: %s\n",
            username, /* dangerous: random bytes from the network! */
            status->pskhex); 
  key->size = status->psk.size;
  key->data = gnutls_malloc(status->psk.size);
  if (!key->data)
    return GNUTLS_E_MEMORY_ERROR;
  memcpy(key->data, status->psk.data, status->psk.size);
  return GNUTLS_E_SUCCESS;
}

void skt_log(int level, const char* data) {
  fprintf(stderr, "S:|<%d>| %s%s", level, data, data[strlen(data)-1] == '\n' ? "" : "\n");
}


int print_qrcode(FILE* f, const QRcode* qrcode) {
  const struct { char *data; size_t size; }  out[] = {
    { .data = "\xe2\x96\x88", .size = 3 }, /* U+2588 FULL BLOCK */
    { .data = "\xe2\x96\x80", .size = 3 }, /* U+2580 UPPER HALF BLOCK */
    { .data = "\xe2\x96\x84", .size = 3 }, /* U+2584 LOWER HALF BLOCK */
    { .data = " ", .size = 1 }, /* U+0020 SPACE */
  };
  const int margin = 2;
  int mx, my;

  if (1 != fwrite("\n", 1, 1, f)) {
    fprintf(stderr, "failed to write start of qrcode\n");
    return -1;
  }
  for (my = 0; my < margin; my++) {
    for (mx = 0; mx < qrcode->width + margin*4; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at upper margin of qrcode\n");
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code in upper margin\n");
      return -1;
    }
  }
  
  for (int iy = 0; iy < qrcode->width; iy+= 2) {
    for (mx = 0; mx < margin*2; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at left margin of qrcode in row %d\n", iy);
        return -1;
      }
    for (int ix = 0; ix < qrcode->width; ix++) {
      int n = (qrcode->data[iy*qrcode->width + ix] & 0x01) << 1;
      if (iy+1 < qrcode->width)
        n += (qrcode->data[(iy+1)*qrcode->width + ix] & 0x01);
      if (1 != fwrite(out[n].data, out[n].size, 1, f)) {
        fprintf(stderr, "failed writing QR code at (%d,%d)\n", ix, iy);
        return -1;
      }
    }
    for (mx = 0; mx < margin*2; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at right margin of qrcode in row %d\n", iy);
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code after line %d\n", iy);
      return -1;
    }
  }
  
  for (my = 0; my < margin; my++) {
    for (mx = 0; mx < qrcode->width + margin*4; mx++)
      if (1 != fwrite(out[0].data, out[0].size, 1, f)) {
        fprintf(stderr, "failed at lower margin of qrcode\n");
        return -1;
      }
    if (1 != fwrite("\n", 1, 1, f)) {
      fprintf(stderr, "failed writing newline into QR code in lower margin\n");
      return -1;
    }
  }

  if (fflush(f))
    fprintf(stderr, "Warning: failed to flush QR code stream: (%d) %s\n", errno, strerror(errno));

  return 0;
}

int print_address_name(struct sockaddr_storage *addr, char *paddr, size_t paddrsz, int *port) {
  if (addr->ss_family == AF_INET6) {
    struct sockaddr_in6 *sa_serv_full;
    sa_serv_full = (struct sockaddr_in6 *)addr;
    if (NULL == inet_ntop(addr->ss_family, &(sa_serv_full->sin6_addr), paddr, paddrsz)) {
      fprintf(stderr, "inet_ntop failed (%d) %s\n", errno, strerror(errno));
      return -1;
    }
    *port = ntohs(sa_serv_full->sin6_port);
  } else if (addr->ss_family == AF_INET) {
    struct sockaddr_in *sa_serv_full;
    sa_serv_full = (struct sockaddr_in *)addr;
    if (NULL == inet_ntop(addr->ss_family, &(sa_serv_full->sin_addr), paddr, paddrsz)) {
      fprintf(stderr, "inet_ntop failed (%d) %s\n", errno, strerror(errno));
      return -1;
    }
    *port = ntohs(sa_serv_full->sin_port);
  } else {
    fprintf(stderr, "unrecognized address family %d\n", addr->ss_family);
    return -1;
  }
  return 0;
}



int main(int argc, const char *argv[]) {
  struct session_status *status;
  int rc;
  gnutls_psk_server_credentials_t creds = NULL;
  gnutls_priority_t priority_cache;
  int optval = 1;
  size_t pskhexsz = sizeof(status->pskhex);
  char urlbuf[INET6_ADDRSTRLEN + 25 + 32];
  int urllen;
  QRcode *qrcode = NULL;
  struct ifaddrs *ifa;
  struct sockaddr *myaddr = NULL;
  int myfamily = 0;
  gpgme_error_t gerr = GPG_ERR_NO_ERROR;
  FILE * inkey = NULL;

  status = session_status_new();
  if (!status) {
    fprintf(stderr, "Failed to initialize status object\n");
    return -1;
  }
      
  

  if (argc > 1) {
    if (!strcmp(argv[1], "-")) {
      inkey = stdin;
    } else {
      inkey = fopen(argv[1], "r");
      if (inkey == NULL)
        fprintf(stderr, "could not read key '%s', instead waiting to receive key: (%d) %s\n",
                argv[1], errno, strerror(errno));
    }
  }
    
  gpgme_check_version (NULL);
  if ((gerr = gpgme_new(&status->gpgctx))) {
    fprintf(stderr, "gpgme_new failed: (%d), %s\n", gerr, gpgme_strerror(gerr));
  }
  
  gnutls_global_set_log_level(LOG_LEVEL);
  gnutls_global_set_log_function(skt_log);
  
  /* choose random number */  
  if ((rc = gnutls_key_generate(&status->psk, PSK_BYTES))) {
    fprintf(stderr, "failed to get randomness: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_hex_encode(&status->psk, status->pskhex, &pskhexsz))) {
    fprintf(stderr, "failed to encode PSK as a hex string: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if (pskhexsz != sizeof(status->pskhex)) {
    fprintf(stderr, "bad calculation for psk size\n");
    return -1;
  }
  for (int ix = 0; ix < sizeof(status->pskhex)-1; ix++)
    status->pskhex[ix] = toupper(status->pskhex[ix]);
  
  /* pick an IP address with getifaddrs instead of using in6addr_any */
  if (getifaddrs(&status->ifap)) {
    fprintf(stderr, "getifaddrs failed: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  for (ifa = status->ifap; ifa; ifa = ifa->ifa_next) {
    char addrstring[INET6_ADDRSTRLEN];
    bool skip = false;
    int family = 0;
    
    if (ifa->ifa_addr) {
      family = ((struct sockaddr_storage*)(ifa->ifa_addr))->ss_family;
      void * ptr = NULL;
      if (family == AF_INET6)
        ptr = &((struct sockaddr_in6*)(ifa->ifa_addr))->sin6_addr;
      else if (family == AF_INET)
        ptr = &((struct sockaddr_in*)(ifa->ifa_addr))->sin_addr;
      else if (family == AF_PACKET) 
        skip = true; /* struct rtnl_link_stats *stats = ifa->ifa_data */
      if (!skip)
        inet_ntop(family, ptr, addrstring, sizeof(addrstring));
      else
        strcpy(addrstring, "<unknown family>");
    } else {
      strcpy(addrstring, "<no address>");
    }
    if (ifa->ifa_flags & IFF_LOOPBACK) {
      if (LOG_LEVEL > 2)
        fprintf(stderr, "skipping %s because it is loopback\n", ifa->ifa_name);
      continue;
    }
    if (!(ifa->ifa_flags & IFF_UP)) {
      if (LOG_LEVEL > 2)
        fprintf(stderr, "skipping %s because it is not up\n", ifa->ifa_name);
      continue;
    }
    if (!skip) {
      if (LOG_LEVEL > 2)
        fprintf(stdout, "%s %s: %s (flags: 0x%x)\n", myaddr==NULL?"*":" ", ifa->ifa_name, addrstring, ifa->ifa_flags);
      /* FIXME: we're just taking the first up, non-loopback address */
      /* be cleverer about prefering wifi, preferring link-local addresses, and RFC1918 addressses. */
      if (myaddr == NULL) {
        myfamily = family;
        myaddr = ifa->ifa_addr;
      }
    }
  }

  if (myfamily == 0) {
    fprintf(stderr, "could not find an acceptable address to bind to.\n");
    return -1;
  }
  
  /* open listening socket */
  status->listen_socket = socket(myfamily, SOCK_STREAM, 0);
  if (status->listen_socket == -1) {
    fprintf(stderr, "failed to allocate a socket of type %d: (%d) %s\n", myfamily, errno, strerror(errno));
    return -1;
  }
  if ((rc = setsockopt(status->listen_socket, SOL_SOCKET, SO_REUSEADDR, (void *) &optval, sizeof(int)))) {
    fprintf(stderr, "failed to set SO_REUSEADDR: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  if ((rc = bind(status->listen_socket, myaddr, myfamily == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)))) {
    fprintf(stderr, "failed to bind: (%d) %s\n", errno, strerror(errno));
    return -1;
  }    
  if ((rc = getsockname(status->listen_socket, (struct sockaddr *) &status->sa_serv_storage, &status->sa_serv_storage_sz))) {
    fprintf(stderr, "failed to getsockname: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  if (status->sa_serv_storage_sz > sizeof(status->sa_serv_storage)) {
    fprintf(stderr, "needed more space (%d) than expected (%zd) for getsockname\n", status->sa_serv_storage_sz, sizeof(status->sa_serv_storage));
    return -1;
  }
  if (status->sa_serv_storage.ss_family != myfamily) {
    fprintf(stderr, "was expecting address family %d after binding, got %d\n", myfamily, status->sa_serv_storage.ss_family);
    return -1;
  }
  if (print_address_name(&status->sa_serv_storage, status->addrp, sizeof(status->addrp), &status->port))
    return -1;
  if ((rc = listen(status->listen_socket, 0))) {
    fprintf(stderr, "failed to listen: (%d) %s\n", errno, strerror(errno));
    return -1;
  }    

  
  /* open tls server connection */
  if ((rc = gnutls_init(&status->session, GNUTLS_SERVER))) {
    fprintf(stderr, "failed to init session: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  gnutls_session_set_ptr(status->session, status);
  if ((rc = gnutls_psk_allocate_server_credentials(&creds))) {
    fprintf(stderr, "failed to allocate PSK credentials: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_psk_set_server_credentials_hint(creds, psk_id_hint))) {
    fprintf(stderr, "failed to set server credentials hint to '%s', ignoring…\n", psk_id_hint);
  }
  if ((rc = gnutls_psk_set_server_known_dh_params(creds, GNUTLS_SEC_PARAM_HIGH))) {
    fprintf(stderr, "failed to set server credentials known DH params: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  gnutls_psk_set_server_credentials_function(creds, get_psk_creds);
  if ((rc = gnutls_credentials_set(status->session, GNUTLS_CRD_PSK, creds))) {
    fprintf(stderr, "failed to assign PSK credentials to GnuTLS server: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_priority_init(&priority_cache, priority, NULL))) {
    fprintf(stderr, "failed to set up GnuTLS priority: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }
  if ((rc = gnutls_priority_set(status->session, priority_cache))) {
    fprintf(stderr, "failed to assign gnutls priority: (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }

  /* construct string */
  urlbuf[sizeof(urlbuf)-1] = 0;
  urllen = snprintf(urlbuf, sizeof(urlbuf)-1, "%s://%s@%s%s%s:%d", schema, status->pskhex, myfamily==AF_INET6?"[":"", status->addrp, myfamily==AF_INET6?"]":"", status->port);
  if (urllen >= (sizeof(urlbuf)-1)) {
    fprintf(stderr, "buffer was somehow truncated.\n");
    return -1;
  }
  if (urllen < 5) {
    fprintf(stderr, "printed URL was somehow way too small (%d).\n", urllen);
    return -1;
  }
  fprintf(stdout, "%s\n", urlbuf);
      
  /* generate qrcode (can't use QR_MODE_AN because of punctuation in URL) */
  qrcode = QRcode_encodeString(urlbuf, 0, QR_ECLEVEL_L, QR_MODE_8, 0);
  if (qrcode == NULL) {
    fprintf(stderr, "failed to encode string as QRcode: (%d) %s\n", errno, strerror(errno));
    return -1;
  }
  
  /* display qrcode */
  if ((rc = print_qrcode(stdout, qrcode))) {
    fprintf(stderr, "failed to print qr code\n");
    return -1;
  }

  /* for test purposes... */
  fprintf(stdout, "gnutls-cli --debug %d --priority %s --port %d --pskusername %s --pskkey %s %s\n",
          LOG_LEVEL, priority, status->port, psk_id_hint, status->pskhex, status->addrp);
  
  /* wait for connection to come in */
  /* FIXME: blocking */
  status->accepted_socket = accept(status->listen_socket, (struct sockaddr *) &status->sa_cli_storage, &status->sa_cli_storage_sz);
  if (close(status->listen_socket)) {
    fprintf(stderr, "error closing listening socket: (%d) %s\n", errno, strerror(errno));
    return -1;
  }

  gnutls_transport_set_int(status->session, status->accepted_socket);

  do {
    rc = gnutls_handshake(status->session); /* FIXME: blocking */
    if (rc && !gnutls_error_is_fatal(rc) && LOG_LEVEL > 2)
      fprintf(stderr, "GnuTLS handshake returned: (%d) %s\n", rc, gnutls_strerror(rc));
  } while (rc < 0 && gnutls_error_is_fatal(rc) == 0);

  if (rc < 0) {
    fprintf(stderr, "TLS Handshake failed (%d) %s\n", rc, gnutls_strerror(rc));
    return -1;
  }

  if (print_address_name(&status->sa_cli_storage, status->caddrp, sizeof(status->caddrp), &status->cport))
    return -1;

  fprintf(stdout, "A connection was made from %s%s%s:%d!\n",
          status->sa_cli_storage.ss_family==AF_INET6?"[":"",
          status->caddrp,
          status->sa_cli_storage.ss_family==AF_INET6?"]":"",
          status->cport
          );

  if (inkey) {
    /* FIXME: send key */
    char data[65536];
    if (LOG_LEVEL > 3)
      fprintf(stderr, "trying to write %s to client\n", (stdin == inkey) ? "standard input" : argv[1]);

    /* read from inkey, send to gnutls */
    while (!feof(inkey)) {
      size_t r;
      r = fread(data, 1, sizeof(data), inkey); /* FIXME: blocking */
      if (ferror(inkey)) {
        fprintf(stderr, "Error reading from input\n");
        return -1;
      } else {
        if (LOG_LEVEL > 3)
          fprintf(stderr, "trying to write %zd octets to client\n", r);
        while (r) {
          rc = GNUTLS_E_AGAIN;
          while (rc == GNUTLS_E_AGAIN || rc == GNUTLS_E_INTERRUPTED) {
            rc = gnutls_record_send(status->session, data, r); /* FIXME: blocking */
            if (rc < 0) {
              if (rc != GNUTLS_E_AGAIN && rc != GNUTLS_E_INTERRUPTED) {
                fprintf(stderr, "gnutls_record_send() failed: (%d) %s\n", rc, gnutls_strerror(rc));
                return -1;
              }
            } else {
              r -= rc;
            }
          }
        }
      }
    }
  } else {
    char data[65536];
    ssize_t datasz;
    /* receive key */
    fprintf(stderr, "waiting to receive key\n");
    /* it's cleaner to do this, but some clients do not like half-open connections: */
    /* gnutls_bye(status->session, GNUTLS_SHUT_WR); */ 
    
    do {
      datasz = gnutls_record_recv(status->session, data, sizeof(data)); /* FIXME: blocking */
      if (datasz > 0) {
        /* writing incoming key to stdout */
        if (1 != fwrite(data, datasz, 1, stdout)) {
          fprintf(stderr, "failed to write incoming record of size %zd\n", datasz);
          return -1;
        }
        /* FIXME: do something more clever with the incoming key (feed
           to gpgme ephemeral context and then prompt to import?) */
      } else if (datasz < 0) {
        fprintf(stderr, "got gnutls error on recv: (%zd) %s\n", datasz, gnutls_strerror(datasz));
        switch (datasz) {
        case GNUTLS_E_REHANDSHAKE:
        case GNUTLS_E_PREMATURE_TERMINATION:
          fprintf(stderr, "closing\n");
          return -1;
          break;;
        }
      }
    } while (datasz);
  }

  /* cleanup */
  session_status_free(status);
  gnutls_priority_deinit(priority_cache);
  gnutls_psk_free_server_credentials(creds);
  QRcode_free(qrcode);
  return 0;
}
